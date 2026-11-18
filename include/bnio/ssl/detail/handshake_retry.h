/**
 * @file handshake_retry.h
 * @brief Factory-driven SSL handshake retry operation and sender.
 */

#pragma once
#ifndef BNIO_SSL_DETAIL_HANDSHAKE_RETRY_H_
#define BNIO_SSL_DETAIL_HANDSHAKE_RETRY_H_

#include <bnio/ssl/base/errors.h>
#include <bnio/ssl/context.h>
#include <bnio/ssl/detail/common.h>
#include <bnio/ssl/detail/handshake_concepts.h>
#include <bnio/ssl/detail/handshake_pause_table.h>
#include <bnio/ssl/detail/state_machine.h>
#include <bnio/ssl/handshake_pause.h>

#include <atomic>
#include <bexec/completion_signatures.hpp>
#include <bexec/detail/operation_storage.hpp>
#include <bexec/receiver.hpp>
#include <bexec/sender.hpp>
#include <cstdint>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::ssl {

/** @cond BNIO_DETAIL */
namespace detail {

/**
 * Whole-operation driver for async_handshake(scheduler, type, factory).
 *
 * Transport pauses run through operation_base; the factory handles only
 * callback-driven pauses. Completes with set_value(std::error_code) or
 * set_stopped(); a factory-returned pause becomes protocol_error.
 */
template <class Scheduler, class NextLayer, class Receiver, class Factory>
class handshake_retry_operation
    : public operation_base<
          handshake_retry_operation<Scheduler, NextLayer, Receiver, Factory>,
          Scheduler, NextLayer, Receiver> {
 public:
  using base = operation_base<
      handshake_retry_operation<Scheduler, NextLayer, Receiver, Factory>,
      Scheduler, NextLayer, Receiver>;
  using receiver_type = std::remove_cvref_t<Receiver>;
  using factory_type = Factory;

  using retry_sender_type = std::remove_cvref_t<
      std::invoke_result_t<factory_type&, const std::error_code&>>;

  /**
   * Receiver handed to the factory's sender.
   *
   * get_env forwards the downstream environment so the stop token reaches the
   * user's work; results are stored in this operation. Accepts only
   * set_value(std::error_code) and set_stopped().
   */
  class retry_receiver {
   public:
    // Named explicitly so get_env's return type is available while the
    // operation class is still incomplete.
    using env_type = decltype(bexec::get_env(std::declval<receiver_type&>()));

    retry_receiver(handshake_retry_operation& operation,
                   std::uint64_t epoch) noexcept
        : operation_(&operation), epoch_(epoch) {}

    [[nodiscard]] env_type get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::error_code ec) noexcept {
      operation_->child_value(epoch_, ec);
    }

    void set_stopped() noexcept { operation_->child_stopped(epoch_); }

   private:
    handshake_retry_operation* operation_;
    std::uint64_t epoch_;
  };

  using retry_operation_type = decltype(bexec::connect(
      std::declval<retry_sender_type>(), std::declval<retry_receiver>()));
  using child_operations_type =
      bexec::detail::operation_storage<bexec::type_list<retry_operation_type>>;

  // Factory invocation, sender move, and connect are the only steps that can
  // throw; take the try/catch route only when one of them can.
  static constexpr bool retry_nothrow =
      std::is_nothrow_invocable_v<factory_type&, const std::error_code&> &&
      std::is_nothrow_move_constructible_v<retry_sender_type> &&
      noexcept(bexec::connect(std::declval<retry_sender_type>(),
                              std::declval<retry_receiver>()));

  handshake_retry_operation(Scheduler scheduler, tcp::stream<NextLayer>& stream,
                            handshake_type type, Factory factory,
                            Receiver receiver)
      : base(std::move(scheduler), stream, std::move(receiver)),
        type_(type),
        factory_(std::move(factory)) {}

  handshake_retry_operation(const handshake_retry_operation&) = delete;
  handshake_retry_operation& operator=(const handshake_retry_operation&) =
      delete;
  handshake_retry_operation(handshake_retry_operation&&) = delete;
  handshake_retry_operation& operator=(handshake_retry_operation&&) = delete;

  /**
   * Runs the first SSL attempt, or reports the invalid-stream error.
   *
   * The factory is never invoked when the stream has no SSL object.
   */
  void on_start() noexcept {
    if (!this->stream_->valid()) {
      // No OpenSSL call happens here, so there is no OpenSSL error to read:
      // report the dedicated no-OpenSSL-error value.
      this->post_complete_error(bnio::ssl::base::make_no_ssl_error());
      return;
    }

    if (type_ == handshake_type::client) {
      SSL_set_connect_state(native_ssl(*this->stream_));
    } else {
      SSL_set_accept_state(native_ssl(*this->stream_));
    }
    kick(driver_action::ssl_attempt);
  }

  /**
   * Receives the staged action once the pending output flush finished.
   *
   * operation_base forwards handshake, finish, fail, and pause here. The action
   * is staged, never executed here: an inline flush completion must not nest
   * another driver frame.
   */
  void resume(resume_action action) noexcept {
    switch (action) {
      case resume_action::handshake:
        // An internal transport step (WANT_READ/WANT_WRITE) made progress:
        // run another SSL attempt.
        kick(driver_action::ssl_attempt);
        return;
      case resume_action::finish:
        kick(driver_action::finish);
        return;
      case resume_action::fail:
        kick(driver_action::fail);
        return;
      case resume_action::pause:
        kick(driver_action::wait);
        return;
      default:
        // Defensive: operation_base never forwards transport_read here.
        // Report a protocol violation rather than re-running the attempt.
        this->post_complete_value(
            std::make_error_code(std::errc::protocol_error));
        return;
    }
  }

  /**
   * Receives a terminal completion staged by operation_base.
   *
   * Only queues it: driver_action::complete posts it as the driver loop's last
   * action, because the schedule post may destroy *this inline.
   */
  void on_terminal_staged() noexcept { kick(driver_action::complete); }

  void deliver_value(std::error_code ec) noexcept {
    bexec::set_value(std::move(this->receiver_), ec);
  }

 private:
  enum class driver_action : int {
    none = 0,
    // Run one SSL_do_handshake attempt (and flush its output).
    ssl_attempt,
    // Invoke the factory for the last callback-driven pause and start its
    // wait sender.
    wait,
    // Process the result stored by the factory child.
    child_done,
    // The pending output flush finished after a successful handshake.
    finish,
    // The pending output flush finished after a fatal SSL error.
    fail,
    // operation_base staged a terminal completion; submit its post.
    complete,
  };

  /**
   * Publishes one action and makes sure a driver processes it.
   *
   * A re-entrant call finds the driver token held and returns; the active
   * driver picks the action up. The seq_cst exchanges prevent a lost wakeup.
   */
  void kick(driver_action action) noexcept {
    pending_.store(action, std::memory_order_seq_cst);
    drive();
  }

  /**
   * Single-winner driver loop.
   *
   * Synchronous rounds run iteratively instead of recursing. Terminal
   * completions are only recorded by their callbacks; driver_action::complete
   * posts as the last action and returns without touching any member.
   */
  void drive() noexcept {
    if (driver_active_.exchange(true, std::memory_order_seq_cst)) {
      return;
    }

    for (;;) {
      const driver_action action =
          pending_.exchange(driver_action::none, std::memory_order_seq_cst);
      switch (action) {
        case driver_action::none:
          if (handle_none()) {
            return;
          }
          break;
        case driver_action::finish:
          handle_finish();
          break;
        case driver_action::fail:
          handle_fail();
          break;
        case driver_action::complete:
          handle_complete();
          return;
        case driver_action::ssl_attempt:
          handle_attempt();
          break;
        case driver_action::wait:
          handle_wait();
          break;
        case driver_action::child_done:
          handle_child_done();
          break;
      }
    }
  }

  /**
   * Second half of the no-lost-wakeup handoff.
   *
   * Releases the token, then re-checks the staged action: either this load sees
   * a concurrent kick, or the kicker's exchange sees the release. Returns true
   * when the driver may return.
   */
  [[nodiscard]] bool handle_none() noexcept {
    driver_active_.store(false, std::memory_order_seq_cst);
    if (pending_.load(std::memory_order_seq_cst) == driver_action::none) {
      return true;
    }
    if (driver_active_.exchange(true, std::memory_order_seq_cst)) {
      return true;
    }
    return false;
  }

  // Success: the output was flushed; stage the empty error code. The terminal
  // hook queues `complete`, posted as the driver's last action.
  void handle_finish() noexcept {
    this->post_complete_value(bnio::ssl::base::empty_error_code);
  }

  // Fatal: the alert was flushed; stage the SSL error captured before it.
  void handle_fail() noexcept {
    this->post_complete_error(this->pending_error_);
  }

  // Terminal post last: the schedule post may complete inline and destroy
  // *this.
  void handle_complete() noexcept { this->submit_staged_completion(); }

  // The attempt may stage finish/fail/pause, or the terminal hook may queue
  // `complete`; the next iteration picks it up.
  void handle_attempt() noexcept { (void)run_ssl_attempt(); }

  void handle_wait() noexcept { (void)start_wait_round(); }

  // A terminal result is left to the next iteration, which posts it;
  // otherwise the driver runs another SSL attempt.
  void handle_child_done() noexcept {
    if (process_child_result()) {
      return;
    }
    pending_.store(driver_action::ssl_attempt, std::memory_order_seq_cst);
  }

  /**
   * Performs one SSL handshake attempt.
   *
   * A failed attempt computes SSL_get_error() once. Transport pauses go to
   * operation_base, which drives the flush/read/write step and resumes through
   * resume(resume_action::handshake); every other result goes to
   * classify_ssl_result(). Returns true when a terminal completion was staged
   * (ZERO_RETURN, with no flush).
   */
  [[nodiscard]] bool run_ssl_attempt() noexcept {
    bnio::ssl::base::clear_errors();
    const int result = SSL_do_handshake(native_ssl(*this->stream_));
    if (result == 1) {
      // Success: flush whatever the handshake queued, then finish.
      this->flush_then(resume_action::finish);
      return false;
    }

    const int error = SSL_get_error(native_ssl(*this->stream_), result);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
      // Transport progress: flush pending output and perform the requested
      // read or write; resume(handshake) then runs another attempt.
      this->handle_ssl_error_value(error, resume_action::handshake);
      return false;
    }
    return classify_ssl_result(error);
  }

  /**
   * Classifies a callback-driven SSL_do_handshake() error.
   *
   * Transport pauses were already filtered by run_ssl_attempt(). Returns true
   * when a terminal completion was staged.
   */
  [[nodiscard]] bool classify_ssl_result(int error) noexcept {
    if (const std::optional<handshake_pause_reason> reason =
            lookup_handshake_pause(error)) {
      // A pause is not an error: do not read the OpenSSL error queue.
      last_pause_ = make_handshake_pause_error(*reason);
      this->flush_then(resume_action::pause);
      return false;
    }

    if (error == SSL_ERROR_ZERO_RETURN) {
      // Orderly close: terminal, no flush (like a transport EOF).
      this->post_complete_value(
          std::make_error_code(std::errc::connection_reset));
      return true;
    }

    // Hard failure: the write BIO may hold a fatal alert (e.g. an ALPN
    // rejection), so flush before reporting the error.
    this->pending_error_ = bnio::ssl::base::last_error();
    this->flush_then(resume_action::fail);
    return false;
  }

  /**
   * Invokes the factory for the last callback-driven pause and starts one wait
   * round.
   *
   * Returns true when this function completed the operation; a factory throw
   * maps to std::errc::protocol_error. The epoch CAS hands the round to either
   * the driver or the child; the loser processes the stored result.
   */
  [[nodiscard]] bool start_wait_round() noexcept {
    // The previous round is done: the epoch handoff guarantees both parties
    // are done with the stored child, so replacing it is safe.
    child_operation_.reset();

    const std::uint64_t child_epoch = epoch_.load(std::memory_order_acquire);
    if constexpr (retry_nothrow) {
      emplace_wait_child(child_epoch);
    } else {
      try {
        emplace_wait_child(child_epoch);
      } catch (...) {
        // A throwing factory or connect is a contract violation; there is no
        // other completion channel.
        this->post_complete_value(
            std::make_error_code(std::errc::protocol_error));
        return true;
      }
    }

    child_operation_.start();

    // Race the child receiver to claim the round; the loser processes the
    // stored completion.
    std::uint64_t expected = child_epoch;
    if (epoch_.compare_exchange_strong(expected, child_epoch + 1,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      return false;
    }

    // The child won (synchronous completion): stage the handoff for this
    // still-active driver.
    kick(driver_action::child_done);
    return false;
  }

  /**
   * Invokes the stored factory (by value, as a non-const lvalue) with the
   * const-lvalue pause code and connects its sender.
   */
  void emplace_wait_child(std::uint64_t child_epoch) {
    child_operation_.template emplace_from<retry_operation_type>(
        // noexcept(retry_nothrow) lets the compiler drop unreachable
        // exception-cleanup edges.
        [this, child_epoch]() noexcept(retry_nothrow) {
          // Non-const factory, const pause code: the value categories the
          // concept checks.
          auto sender = factory_(std::as_const(last_pause_));
          return bexec::connect(std::move(sender),
                                retry_receiver(*this, child_epoch));
        });
  }

  /**
   * Handles the result stored by the factory child.
   *
   * Returns true when the operation completed, false to run another SSL
   * attempt.
   */
  [[nodiscard]] bool process_child_result() noexcept {
    if (child_stopped_) {
      this->post_complete_stopped();
      return true;
    }
    if (child_error_) {
      if (is_handshake_pause(child_error_)) {
        // A pause from the factory would leak to the public receiver or loop
        // forever; report a protocol violation instead.
        this->post_complete_value(
            std::make_error_code(std::errc::protocol_error));
      } else {
        this->post_complete_value(child_error_);
      }
      return true;
    }
    return false;
  }

  void child_value(std::uint64_t child_epoch, std::error_code ec) noexcept {
    child_error_ = ec;
    child_stopped_ = false;
    finish_child(child_epoch);
  }

  void child_stopped(std::uint64_t child_epoch) noexcept {
    child_stopped_ = true;
    finish_child(child_epoch);
  }

  /**
   * Completes one factory-child round.
   *
   * The winner's acq_rel CAS publishes the stored result to the loser's
   * acquire. A loser never drives the loop while a driver is active: it only
   * stages child_done.
   */
  void finish_child(std::uint64_t child_epoch) noexcept {
    std::uint64_t expected = child_epoch;
    if (epoch_.compare_exchange_strong(expected, child_epoch + 1,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      // The child won: the driver's failed (or yet to run) CAS observes the
      // stored completion.
      return;
    }
    kick(driver_action::child_done);
  }

  handshake_type type_;
  factory_type factory_;

  // Last callback-driven pause code handed to the factory (written before the
  // flush, read by the child construction only while the driver is active).
  std::error_code last_pause_ = bnio::ssl::base::empty_error_code;

  // Published before the epoch CAS (release) and read by the loser of the
  // race only (acquire).
  std::error_code child_error_ = bnio::ssl::base::empty_error_code;
  bool child_stopped_ = false;

  std::atomic<std::uint64_t> epoch_{0};
  std::atomic<bool> driver_active_{false};
  std::atomic<driver_action> pending_{driver_action::none};

  // Declared last: it owns the factory child operation, which refers back to
  // this operation state.
  child_operations_type child_operation_;
};

/**
 * Sender for async_handshake(scheduler, type, factory); completes with
 * set_value(std::error_code) or set_stopped() only. State is inline, so
 * connect does not allocate.
 */
template <class Scheduler, class NextLayer, class Factory>
  requires handshake_retry_factory<Factory>
class handshake_retry_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                   bexec::set_stopped_t()>;
  using factory_type = std::remove_cvref_t<Factory>;

  static_assert(
      completion_signatures::template count_of<bexec::set_value_t>() == 1,
      "the retry sender must advertise exactly one value signature");
  static_assert(
      completion_signatures::template count_of<bexec::set_stopped_t>() == 1,
      "the retry sender must advertise exactly one stopped signature");
  static_assert(
      completion_signatures::template count_of<bexec::set_error_t>() == 0,
      "the retry sender must not advertise an error signature");

  handshake_retry_sender(Scheduler scheduler, tcp::stream<NextLayer>& stream,
                         handshake_type type, factory_type factory)
      : scheduler_(std::move(scheduler)),
        stream_(&stream),
        type_(type),
        factory_(std::move(factory)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return handshake_retry_operation<
        Scheduler, NextLayer, std::remove_cvref_t<Receiver>, factory_type>(
        std::move(scheduler_), *stream_, type_, std::move(factory_),
        std::move(receiver));
  }

  template <class Receiver>
    requires(std::copy_constructible<Scheduler> &&
             std::copy_constructible<factory_type>)
  auto connect(Receiver receiver) const& {
    return handshake_retry_operation<
        Scheduler, NextLayer, std::remove_cvref_t<Receiver>, factory_type>(
        scheduler_, *stream_, type_, factory_, std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  tcp::stream<NextLayer>* stream_;
  handshake_type type_;
  factory_type factory_;
};

}  // namespace detail
/** @endcond */

}  // namespace bnio::ssl

#endif  // BNIO_SSL_DETAIL_HANDSHAKE_RETRY_H_
