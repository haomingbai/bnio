/**
 * @file io_uring_receiver_operation.h
 * @brief Internal io_uring receiver operation state.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_DETAIL_IO_URING_RECEIVER_OPERATION_H_
#define BNIO_ASYNC_IO_LINUX_DETAIL_IO_URING_RECEIVER_OPERATION_H_

#include <bnio/async_io/linux/io_uring_context_base.h>
#include <bnio/detail/error_code.h>

#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cerrno>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::async_io::linux_native {

namespace detail {

/**
 * Completion channel selected for receiver delivery.
 *
 * set_value(ec, ...) is the universal observable exit (success, cancel,
 * recoverable failure); set_stopped is reserved for stop-token cancellation.
 */
enum class io_uring_receiver_completion {
  /** Complete with set_value(empty ec, result, flags). */
  value,

  /** Complete with set_value(ec, result, flags); ec carries a recoverable
   *  error (cancel or SQE preparation failure). */
  value_with_ec,

  /** Completion marked as stopped; token arbitration in execute() picks
   *  set_stopped for a cancelled receiver stop token, otherwise
   *  set_value(operation_canceled, 0, 0). Marked by complete_submit_stopped()
   *  and by start_io()'s stop-token pre-check. */
  stopped,
};

/** Translates io_uring completions into receiver signals. */
template <class Receiver>
class io_uring_receiver_operation : public io_uring_io_operation_base {
 public:
  /** Disabled: operations are queued intrusively. */
  io_uring_receiver_operation(const io_uring_receiver_operation&) = delete;

  /** Disabled: operations are queued intrusively. */
  io_uring_receiver_operation& operator=(const io_uring_receiver_operation&) =
      delete;

  /** Disabled: operations are queued intrusively. */
  io_uring_receiver_operation(io_uring_receiver_operation&&) = delete;

  /** Disabled: operations are queued intrusively. */
  io_uring_receiver_operation& operator=(io_uring_receiver_operation&&) =
      delete;

  /** Destroys the operation without delivering an additional signal. */
  ~io_uring_receiver_operation() noexcept override = default;

  /** Delivers the selected completion signal.
   *
   *  set_stopped is emitted iff the receiver's stop token is cancelled
   *  (including a token that won against io_context::stop()); otherwise the
   *  abort exits through set_value(operation_canceled, 0, 0).
   */
  void execute() noexcept override {
    switch (completion_) {
      case io_uring_receiver_completion::value:
        // The CQE handler only updates result/flags, never completion_; a
        // negative result must re-derive ec or the error would be masked.
        if (result < 0) {
          bexec::set_value(std::move(receiver_),
                           std::error_code(-result, std::generic_category()),
                           result, flags);
        } else {
          bexec::set_value(std::move(receiver_), bnio::detail::empty_error_code,
                           result, flags);
        }
        break;
      case io_uring_receiver_completion::value_with_ec:
        bexec::set_value(std::move(receiver_), error_, result, flags);
        break;
      case io_uring_receiver_completion::stopped:
        // Token arbitration: set_stopped only for a cancelled stop token;
        // an io_context::stop() abort delivers operation_canceled instead.
        if (stop_requested()) {
          bexec::set_stopped(std::move(receiver_));
        } else {
          bexec::set_value(std::move(receiver_),
                           std::error_code(ECANCELED, std::generic_category()),
                           0, 0);
        }
        break;
    }
  }

  void complete_submit_error(int result_code) noexcept override {
    complete_with_ec(result_code);
  }

  void complete_submit_stopped() noexcept override { complete_with_stopped(); }

  /** -EAGAIN rearm flag; see io_uring_io_operation_base::rearm_on_eagain(). */
  [[nodiscard]] bool rearm_on_eagain() const noexcept override {
    return rearm_on_eagain_;
  }

 protected:
  /** Creates a receiver operation associated with an io_uring context. */
  io_uring_receiver_operation(io_uring_context& context, Receiver receiver)
      : context_(&context), receiver_(std::move(receiver)) {}

  /** Whether the receiver environment has requested cancellation. */
  [[nodiscard]] bool stop_requested() const noexcept {
    auto env = bexec::get_env(receiver_);
    auto token = bexec::query(env, bexec::get_stop_token);
    return token.stop_requested();
  }

  /** Opts into -EAGAIN re-submission: a transient would-block CQE is
   *  re-submitted through the I/O queue instead of delivered as a terminal
   *  error. Read/write-class operations call this from their constructor. */
  void enable_eagain_rearm() noexcept { rearm_on_eagain_ = true; }

  /** Selects set_value(empty ec, ...) completion. */
  void complete_with_value() noexcept {
    completion_ = io_uring_receiver_completion::value;
  }

  /** Selects set_value(ec, ...) with a recoverable error (SQE preparation
   *  failure via complete_submit_error). Stop-token cancellation does not
   *  route here; start_io() marks those stopped. */
  void complete_with_ec(int result_code) noexcept {
    completion_ = io_uring_receiver_completion::value_with_ec;
    error_ = std::error_code(-result_code, std::generic_category());
  }

  /** Marks this completion stopped; execute() arbitrates between
   *  set_stopped (cancelled token) and set_value(operation_canceled, 0, 0).
   *  Reached from complete_submit_stopped() and start_io()'s pre-check. */
  void complete_with_stopped() noexcept {
    completion_ = io_uring_receiver_completion::stopped;
  }

  /** Starts the operation, or posts an immediate completion when the stop
   *  token is already cancelled (marked stopped; execute() then emits
   *  set_stopped). An io_context::stop() abort without a cancelled token
   *  delivers set_value(operation_canceled, ...). */
  template <class Operation>
  void start_io(Operation& operation) noexcept {
    if (stop_requested()) {
      complete_with_stopped();
      (void)context_->post(operation);
      return;
    }

    complete_with_value();
    context_->publish_io(operation);
  }

  /** Context that passively prepares and completes the operation. */
  io_uring_context* context_;

  /** Receiver completed by this operation. */
  std::remove_cvref_t<Receiver> receiver_;

  /** Completion channel selected before execute() runs. */
  io_uring_receiver_completion completion_ =
      io_uring_receiver_completion::value;

  /** Error passed to set_value() when completion_ is value_with_ec. */
  std::error_code error_ = bnio::detail::empty_error_code;

  /** Whether a -EAGAIN CQE re-submits; see enable_eagain_rearm(). */
  bool rearm_on_eagain_ = false;
};

}  // namespace detail

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_DETAIL_IO_URING_RECEIVER_OPERATION_H_
