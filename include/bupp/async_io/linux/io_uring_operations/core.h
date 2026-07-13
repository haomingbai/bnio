#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_OPERATIONS_CORE_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_OPERATIONS_CORE_H_

#include <bupp/async_io/linux/detail/io_uring_receiver_operation.h>
#include <bupp/async_io/linux/io_uring_operations/helpers.h>
#include <bupp/base/linux/submission_queue_entry.h>

#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <chrono>
#include <type_traits>
#include <utility>

namespace bupp::async_io::linux_native {

/**
 * Operation that posts a receiver completion onto an io_uring_context.
 */
template <class Receiver>
class io_uring_post_operation : public io_uring_operation_base {
 public:
  /**
   * Creates a post operation for a context and receiver.
   */
  io_uring_post_operation(io_uring_context& context, Receiver receiver)
      : context_(&context), receiver_(std::move(receiver)) {}

  /**
   * Copy construction is disabled because operations are queued intrusively.
   */
  io_uring_post_operation(const io_uring_post_operation&) = delete;

  /**
   * Copy assignment is disabled because operations are queued intrusively.
   */
  io_uring_post_operation& operator=(const io_uring_post_operation&) = delete;

  /**
   * Move construction is disabled because operations are queued intrusively.
   */
  io_uring_post_operation(io_uring_post_operation&&) = delete;

  /**
   * Move assignment is disabled because operations are queued intrusively.
   */
  io_uring_post_operation& operator=(io_uring_post_operation&&) = delete;

  /**
   * Destroys the operation without completing the receiver.
   */
  ~io_uring_post_operation() noexcept override = default;

  /**
   * Delivers set_value or set_stopped to the receiver.
   */
  void execute() noexcept override {
    if (stopped_) {
      bexec::set_stopped(std::move(receiver_));
    } else {
      bexec::set_value(std::move(receiver_));
    }
  }

  /**
   * Posts this operation to the context run loop.
   */
  void start() noexcept {
    auto env = bexec::get_env(receiver_);
    auto token = bexec::query(env, bexec::get_stop_token);
    stopped_ = token.stop_requested();
    (void)context_->post(*this);
  }

 private:
  io_uring_context* context_;
  std::remove_cvref_t<Receiver> receiver_;
  bool stopped_ = false;
};

/**
 * Operation representing an io_uring no-op request.
 */
template <class Receiver>
class io_uring_nop_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a no-op operation for a context and receiver.
   */
  io_uring_nop_operation(io_uring_context& context, Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)) {}

  /**
   * Prepares the no-op SQE.
   *
   * @see io_uring_prep_nop
   */
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_nop();
  }

  /**
   * Starts the no-op operation.
   */
  void start() noexcept { this->start_io(*this); }
};

/**
 * Operation representing an io_uring timeout request.
 */
template <class Receiver>
class io_uring_timeout_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a timeout operation from a chrono duration.
   */
  template <class Rep, class Period>
  io_uring_timeout_operation(io_uring_context& context,
                             std::chrono::duration<Rep, Period> timeout,
                             unsigned count, unsigned timeout_flags,
                             Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        timeout_(detail::to_kernel_timespec(timeout)),
        count_(count),
        timeout_flags_(timeout_flags) {}

  /**
   * Creates a timeout operation from a chrono time point.
   */
  template <class Clock, class Duration>
  io_uring_timeout_operation(io_uring_context& context,
                             std::chrono::time_point<Clock, Duration> timeout,
                             unsigned count, unsigned timeout_flags,
                             Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        timeout_(detail::to_kernel_timespec(timeout)),
        count_(count),
        timeout_flags_(timeout_flags) {}

  /**
   * Prepares the timeout SQE.
   *
   * @see io_uring_prep_timeout
   */
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_timeout(&timeout_, count_, timeout_flags_);
  }

  /**
   * Starts the timeout operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  __kernel_timespec timeout_;
  unsigned count_;
  unsigned timeout_flags_;
};

/**
 * Operation that lets a caller prepare a raw io_uring SQE.
 */
template <class Receiver, class Prepare>
class io_uring_raw_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a raw operation from a caller-provided prepare function.
   */
  io_uring_raw_operation(io_uring_context& context, Prepare prepare,
                         Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        prepare_(std::move(prepare)) {}

  /**
   * Invokes the caller-provided prepare function for the SQE.
   */
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    prepare_(sqe);
  }

  /**
   * Starts the raw operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  std::remove_cvref_t<Prepare> prepare_;
};

}  // namespace bupp::async_io::linux_native

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_OPERATIONS_CORE_H_
