/**
 * @file handshake_wait.h
 * @brief Detail sender performing one BIO-level transport step for a pause.
 */

#pragma once
#ifndef BNIO_SSL_DETAIL_HANDSHAKE_WAIT_H_
#define BNIO_SSL_DETAIL_HANDSHAKE_WAIT_H_

#include <bnio/ssl/base/errors.h>
#include <bnio/ssl/detail/common.h>
#include <bnio/ssl/handshake_pause.h>
#include <openssl/bio.h>

#include <atomic>
#include <bexec/completion_signatures.hpp>
#include <bexec/detail/operation_storage.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/receiver.hpp>
#include <bexec/sender.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::ssl {

/** @cond BNIO_DETAIL */
namespace detail {

/**
 * One round of transport work selected by a handshake pause.
 *
 * read waits for encrypted bytes and commits them into the stream's read BIO;
 * write drains queued output from the write BIO to the transport until empty.
 * Child operations are stored inline in operation_storage (no heap).
 *
 * The public receiver only observes set_value(std::error_code) or
 * set_stopped(); transport values and errors both travel through set_value.
 * Non-pause codes complete with std::errc::invalid_argument, unsupported pause
 * reasons with std::errc::operation_not_supported, and an invalid stream with
 * the dedicated no-OpenSSL-error value (without touching a BIO).
 *
 * Synchronization uses the bexec repeat_until epoch protocol; see drive().
 */
template <class Scheduler, class NextLayer, class Receiver>
class handshake_wait_operation {
 public:
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using receiver_type = std::remove_cvref_t<Receiver>;

  class child_receiver {
   public:
    // Named explicitly so get_env's return type is available while the
    // operation class is still incomplete. The downstream environment is
    // forwarded so the stop token reaches the transport operation.
    using env_type = decltype(bexec::get_env(std::declval<receiver_type&>()));

    child_receiver(handshake_wait_operation& operation,
                   std::uint64_t epoch) noexcept
        : operation_(&operation), epoch_(epoch) {}

    [[nodiscard]] env_type get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::error_code ec, std::size_t bytes) noexcept {
      operation_->child_value(epoch_, ec, bytes);
    }

    void set_stopped() noexcept { operation_->child_stopped(epoch_); }

   private:
    handshake_wait_operation* operation_;
    std::uint64_t epoch_;
  };

  using read_sender_type = decltype(make_transport_read_sender(
      std::declval<scheduler_type&>(), std::declval<tcp::stream<NextLayer>&>(),
      static_cast<void*>(nullptr), std::size_t{}));
  using write_sender_type = decltype(make_transport_write_sender(
      std::declval<scheduler_type&>(), std::declval<tcp::stream<NextLayer>&>(),
      static_cast<const void*>(nullptr), std::size_t{}));
  using read_operation_type = decltype(bexec::connect(
      std::declval<read_sender_type>(), std::declval<child_receiver>()));
  using write_operation_type = decltype(bexec::connect(
      std::declval<write_sender_type>(), std::declval<child_receiver>()));
  using child_operations_type = bexec::detail::operation_storage<
      bexec::type_list<read_operation_type, write_operation_type>>;

  handshake_wait_operation(Scheduler scheduler, tcp::stream<NextLayer>& stream,
                           std::error_code pause, Receiver receiver)
      : scheduler_(std::move(scheduler)),
        stream_(&stream),
        pause_(pause),
        receiver_(std::move(receiver)) {}

  handshake_wait_operation(const handshake_wait_operation&) = delete;
  handshake_wait_operation& operator=(const handshake_wait_operation&) = delete;
  handshake_wait_operation(handshake_wait_operation&&) = delete;
  handshake_wait_operation& operator=(handshake_wait_operation&&) = delete;

  void start() noexcept {
    if (!stream_->valid()) {
      // No BIO exists without an SSL object, so no OpenSSL call happens here:
      // report the dedicated no-OpenSSL-error value.
      complete_value(base::make_no_ssl_error());
      return;
    }

    if (!is_handshake_pause(pause_)) {
      complete_value(std::make_error_code(std::errc::invalid_argument));
      return;
    }

    switch (handshake_pause_reason_of(pause_)) {
      case handshake_pause_reason::want_read:
        direction_ = wait_direction::read;
        drive();
        return;
      case handshake_pause_reason::want_write:
        direction_ = wait_direction::write;
        drive();
        return;
      default:
        // Callback-driven pauses need user-supplied asynchronous work; this
        // BIO helper cannot make progress for them. Unknown nonzero pause
        // values land here too.
        complete_value(
            std::make_error_code(std::errc::operation_not_supported));
        return;
    }
  }

 private:
  enum class wait_direction {
    read,
    write,
  };

  /**
   * Runs wait steps until the round completes.
   *
   * Each step stages one child operation, starts it, then races the child
   * receiver for the epoch. The loser processes the stored completion; the
   * winner returns to its own stack.
   */
  void drive() noexcept {
    for (;;) {
      // The previous child is finished: the epoch handoff guarantees the
      // driver and the child receiver never both touch the stored completion,
      // so replacing it here is safe (the after-signaling rule).
      child_operations_.reset();

      const std::uint64_t child_epoch = epoch_.load(std::memory_order_acquire);
      const bool staged = direction_ == wait_direction::read
                              ? handle_read(child_epoch)
                              : handle_write(child_epoch);
      if (!staged) {
        return;
      }

      child_operations_.start();

      // Race the child receiver to claim the round. The acq_rel success
      // publishes the stored result; the acquire failure observes the
      // winner's publication.
      std::uint64_t expected = child_epoch;
      if (epoch_.compare_exchange_strong(expected, child_epoch + 1,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        return;
      }

      if (!process_child_completion()) {
        return;
      }
    }
  }

  /**
   * Stages one read child and waits for its bytes.
   *
   * Returns false when the step already completed (the read BIO cannot offer
   * a staging buffer).
   */
  [[nodiscard]] bool handle_read(std::uint64_t child_epoch) noexcept {
    base::clear_errors();
    char* data = nullptr;
    const int available = BIO_nwrite0(read_bio(*stream_), &data);
    if (available <= 0) {
      // last_error() reports the recorded OpenSSL error, or the dedicated
      // no-OpenSSL-error value when the failure never reached the queue.
      complete_value(base::last_error());
      return false;
    }

    transport_data_ = data;
    transport_size_ = static_cast<std::size_t>(available);
    child_operations_.template emplace_from<read_operation_type>(
        [this, child_epoch] {
          return bexec::connect(
              make_transport_read_sender(scheduler_, *stream_, transport_data_,
                                         transport_size_),
              child_receiver(*this, child_epoch));
        });
    return true;
  }

  /**
   * Stages one write child to drain the write BIO.
   *
   * Returns false when the step already completed (nothing left to flush).
   */
  [[nodiscard]] bool handle_write(std::uint64_t child_epoch) noexcept {
    // BIO_nread0 on a BIO pair returns the pending output length, 0 when the
    // writing half was shut down, and -1 when no data is available (the
    // normal empty state, not an error). Neither 0 nor -1 is an OpenSSL
    // failure, so the error queue is intentionally not consulted.
    char* data = nullptr;
    const int available = BIO_nread0(write_bio(*stream_), &data);
    if (available <= 0) {
      complete_value(base::empty_error_code);
      return false;
    }

    transport_data_ = data;
    transport_size_ = static_cast<std::size_t>(available);
    child_operations_.template emplace_from<write_operation_type>(
        [this, child_epoch] {
          return bexec::connect(
              make_transport_write_sender(scheduler_, *stream_, transport_data_,
                                          transport_size_),
              child_receiver(*this, child_epoch));
        });
    return true;
  }

  void child_value(std::uint64_t child_epoch, std::error_code ec,
                   std::size_t bytes) noexcept {
    child_error_ = ec;
    child_bytes_ = bytes;
    child_stopped_ = false;
    finish_child(child_epoch);
  }

  void child_stopped(std::uint64_t child_epoch) noexcept {
    child_stopped_ = true;
    finish_child(child_epoch);
  }

  void finish_child(std::uint64_t child_epoch) noexcept {
    std::uint64_t expected = child_epoch;
    if (epoch_.compare_exchange_strong(expected, child_epoch + 1,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      // The child won: the driver's failed (or yet to run) CAS observes the
      // stored completion.
      return;
    }
    if (process_child_completion()) {
      drive();
    }
  }

  /**
   * Handles the completion stored by the winner of the round.
   *
   * Returns true when the write direction consumed one chunk and the driver
   * should continue draining; false when a terminal completion was delivered.
   */
  [[nodiscard]] bool process_child_completion() noexcept {
    if (child_stopped_) {
      complete_stopped();
      return false;
    }
    if (child_error_) {
      complete_value(child_error_);
      return false;
    }
    if (child_bytes_ == 0) {
      // EOF (empty ec with zero bytes) would otherwise spin forever on a dead
      // peer: report connection_reset instead.
      complete_value(std::make_error_code(std::errc::connection_reset));
      return false;
    }

    if (direction_ == wait_direction::read) {
      commit_read();
      return false;
    }
    return consume_write();
  }

  // Commits the bytes received into the read BIO and completes the step.
  void commit_read() noexcept {
    base::clear_errors();
    char* data = nullptr;
    const int committed =
        BIO_nwrite(read_bio(*stream_), &data, bounded_int_size(child_bytes_));
    if (committed != static_cast<int>(child_bytes_)) {
      complete_value(base::last_error());
      return;
    }
    complete_value(base::empty_error_code);
  }

  // Consumes what the transport accepted. Returns true when the write BIO
  // still has output to drain; a short write leaves the rest readable through
  // BIO_nread0.
  [[nodiscard]] bool consume_write() noexcept {
    base::clear_errors();
    char* data = nullptr;
    const int consumed =
        BIO_nread(write_bio(*stream_), &data, bounded_int_size(child_bytes_));
    if (consumed != static_cast<int>(child_bytes_)) {
      complete_value(base::last_error());
      return false;
    }
    return true;
  }

  // Delivers the terminal value; the receiver may destroy *this here.
  void complete_value(std::error_code ec) noexcept {
    bexec::set_value(std::move(receiver_), ec);
  }

  // Delivers the terminal stopped signal; see complete_value.
  void complete_stopped() noexcept { bexec::set_stopped(std::move(receiver_)); }

  scheduler_type scheduler_;
  tcp::stream<NextLayer>* stream_;
  std::error_code pause_;
  receiver_type receiver_;

  // Published before the epoch CAS and read by the loser of the race only.
  std::error_code child_error_ = base::empty_error_code;
  std::size_t child_bytes_ = 0;
  bool child_stopped_ = false;

  wait_direction direction_ = wait_direction::read;
  char* transport_data_ = nullptr;
  std::size_t transport_size_ = 0;
  std::atomic<std::uint64_t> epoch_{0};

  // Declared last: it owns child operations that refer back to this state.
  child_operations_type child_operations_;
};

/**
 * Sender returned by make_handshake_wait_sender.
 *
 * Completes with exactly set_value(std::error_code) or set_stopped(); there is
 * no set_error channel. Connecting an lvalue requires a copyable scheduler; a
 * prvalue always connects.
 */
template <class Scheduler, class NextLayer>
class handshake_wait_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                   bexec::set_stopped_t()>;

  handshake_wait_sender(Scheduler scheduler, tcp::stream<NextLayer>& stream,
                        std::error_code pause)
      : scheduler_(std::move(scheduler)), stream_(&stream), pause_(pause) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return handshake_wait_operation<Scheduler, NextLayer,
                                    std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), *stream_, pause_, std::move(receiver));
  }

  template <class Receiver>
    requires std::copy_constructible<Scheduler>
  auto connect(Receiver receiver) const& {
    return handshake_wait_operation<Scheduler, NextLayer,
                                    std::remove_cvref_t<Receiver>>(
        scheduler_, *stream_, pause_, std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  tcp::stream<NextLayer>* stream_;
  std::error_code pause_;
};

}  // namespace detail
/** @endcond */

}  // namespace bnio::ssl

#endif  // BNIO_SSL_DETAIL_HANDSHAKE_WAIT_H_
