#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_DETAIL_IO_URING_RECEIVER_OPERATION_H_
#define BUPP_ASYNC_IO_LINUX_DETAIL_IO_URING_RECEIVER_OPERATION_H_

#include <bupp/async_io/linux/io_uring_context_base.h>

#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bupp::async_io::linux_native {

namespace detail {

/**
 * Completion channel selected for receiver delivery.
 */
enum class io_uring_receiver_completion {
  /**
   * Complete the receiver with set_value.
   */
  value,

  /**
   * Complete the receiver with set_error.
   */
  error,

  /**
   * Complete the receiver with set_stopped.
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
   */
  void execute() noexcept override {
    switch (completion_) {
      case io_uring_receiver_completion::value:
        bexec::set_value(std::move(receiver_), result, flags);
        break;
      case io_uring_receiver_completion::error:
        bexec::set_error(std::move(receiver_), error_);
        break;
      case io_uring_receiver_completion::stopped:
        bexec::set_stopped(std::move(receiver_));
        break;
    }
  }

  void complete_submit_error(int result_code) noexcept override {
    complete_with_error(result_code);
  }

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
   * Selects set_value completion for this operation.
   */
  void complete_with_value() noexcept {
    completion_ = io_uring_receiver_completion::value;
  }

  /**
   * Selects set_error completion for this operation.
   */
  void complete_with_error(int result_code) noexcept {
    completion_ = io_uring_receiver_completion::error;
    error_ = std::error_code(-result_code, std::generic_category());
  }

  /**
   * Selects set_stopped completion for this operation.
   */
  void complete_with_stopped() noexcept {
    completion_ = io_uring_receiver_completion::stopped;
  }

  /**
   * Starts an io_uring operation or posts an immediate completion.
   */
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
   * Error delivered when completion_ is error.
   */
  std::error_code error_;
};

}  // namespace detail

}  // namespace bupp::async_io::linux_native

#endif  // BUPP_ASYNC_IO_LINUX_DETAIL_IO_URING_RECEIVER_OPERATION_H_
