/**
 * @file io_uring_receiver_operation.h
 * @brief Internal io_uring receiver operation state.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_DETAIL_IO_URING_RECEIVER_OPERATION_H_
#define BNIO_ASYNC_IO_LINUX_DETAIL_IO_URING_RECEIVER_OPERATION_H_

#include <bnio/async_io/linux/io_uring_context_base.h>

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
 * Per completion-semantics contract: set_value(ec, ...) is the universal
 * observable exit (success, cancel, recoverable failure); set_stopped is
 * reserved exclusively for io_context::stop() aborting inflight I/O.
 */
enum class io_uring_receiver_completion {
  /**
   * Complete the receiver with set_value(empty ec, result, flags).
   */
  value,

  /**
   * Complete the receiver with set_value(ec, result, flags) where ec
   * carries a recoverable error (cancel or SQE preparation failure).
   */
  value_with_ec,

  /**
   * Complete the receiver with set_stopped. Selected ONLY by
   * complete_submit_stopped() when io_context::stop() aborts this op.
   */
  stopped,
};

/**
 * Base operation that translates io_uring completions into receiver signals.
 */
template <class Receiver>
class io_uring_receiver_operation : public io_uring_io_operation_base {
 public:
  /**
   * Copy construction is disabled because operations are queued intrusively.
   */
  io_uring_receiver_operation(const io_uring_receiver_operation&) = delete;

  /**
   * Copy assignment is disabled because operations are queued intrusively.
   */
  io_uring_receiver_operation& operator=(const io_uring_receiver_operation&) =
      delete;

  /**
   * Move construction is disabled because operations are queued intrusively.
   */
  io_uring_receiver_operation(io_uring_receiver_operation&&) = delete;

  /**
   * Move assignment is disabled because operations are queued intrusively.
   */
  io_uring_receiver_operation& operator=(io_uring_receiver_operation&&) =
      delete;

  /**
   * Destroys the operation without delivering an additional signal.
   */
  ~io_uring_receiver_operation() noexcept override = default;

  /**
   * Delivers the selected completion signal to the receiver.
   *
   * Both `value` and `value_with_ec` exit through set_value; only the
   * leading std::error_code differs (empty for success). `stopped`
   * exits through set_stopped and is reachable only when
   * io_context::stop() aborted this inflight operation.
   */
  void execute() noexcept override {
    switch (completion_) {
      case io_uring_receiver_completion::value:
        // §9.2 guard: CQE 处理器只更新 result/flags,不重分类 completion_。
        // 当 result < 0 时(-errno)必须从 result 重新派生 ec,否则错误被掩盖。
        if (result < 0) {
          bexec::set_value(std::move(receiver_),
                           std::error_code(-result, std::generic_category()),
                           result, flags);
        } else {
          bexec::set_value(std::move(receiver_), std::error_code{}, result,
                           flags);
        }
        break;
      case io_uring_receiver_completion::value_with_ec:
        bexec::set_value(std::move(receiver_), error_, result, flags);
        break;
      case io_uring_receiver_completion::stopped:
        bexec::set_stopped(std::move(receiver_));
        break;
    }
  }

  void complete_submit_error(int result_code) noexcept override {
    complete_with_ec(result_code);
  }

  void complete_submit_stopped() noexcept override { complete_with_stopped(); }

 protected:
  /**
   * Creates a receiver operation associated with an io_uring context.
   */
  io_uring_receiver_operation(io_uring_context& context, Receiver receiver)
      : context_(&context), receiver_(std::move(receiver)) {}

  /**
   * Returns whether the receiver environment has requested cancellation.
   */
  [[nodiscard]] bool stop_requested() const noexcept {
    auto env = bexec::get_env(receiver_);
    auto token = bexec::query(env, bexec::get_stop_token);
    return token.stop_requested();
  }

  /**
   * Selects set_value(empty ec, ...) completion for this operation.
   */
  void complete_with_value() noexcept {
    completion_ = io_uring_receiver_completion::value;
  }

  /**
   * Selects set_value(ec, ...) completion where ec carries a recoverable
   * error (user cancel via stop_token, or SQE preparation failure reported
   * through complete_submit_error).
   */
  void complete_with_ec(int result_code) noexcept {
    completion_ = io_uring_receiver_completion::value_with_ec;
    error_ = std::error_code(-result_code, std::generic_category());
  }

  /**
   * Selects set_stopped completion for this operation.
   *
   * Reached only via complete_submit_stopped(), i.e. when
   * io_context::stop() aborts this inflight I/O operation.
   */
  void complete_with_stopped() noexcept {
    completion_ = io_uring_receiver_completion::stopped;
  }

  /**
   * Starts an io_uring operation or posts an immediate completion.
   *
   * A user-requested cancel (stop_token) now completes through
   * set_value(operation_canceled, ...) rather than set_stopped; the
   * latter is reserved for io_context::stop().
   */
  template <class Operation>
  void start_io(Operation& operation) noexcept {
    if (stop_requested()) {
      complete_with_ec(-ECANCELED);
      (void)context_->post(operation);
      return;
    }

    complete_with_value();
    context_->publish_io(operation);
  }

  /**
   * Context that passively prepares and completes the operation.
   */
  io_uring_context* context_;

  /**
   * Receiver completed by this operation.
   */
  std::remove_cvref_t<Receiver> receiver_;

  /**
   * Completion channel selected before execute runs.
   */
  io_uring_receiver_completion completion_ =
      io_uring_receiver_completion::value;

  /**
   * Error delivered as the leading argument of set_value when
   * completion_ is value_with_ec. Empty for the value channel.
   */
  std::error_code error_;
};

}  // namespace detail

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_DETAIL_IO_URING_RECEIVER_OPERATION_H_
