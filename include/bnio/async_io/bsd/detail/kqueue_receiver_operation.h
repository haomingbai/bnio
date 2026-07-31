/**
 * @file kqueue_receiver_operation.h
 * @brief Internal kqueue receiver operation state.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_DETAIL_KQUEUE_RECEIVER_OPERATION_H_
#define BNIO_ASYNC_IO_BSD_DETAIL_KQUEUE_RECEIVER_OPERATION_H_

#include <bnio/async_io/bsd/kqueue_context_base.h>

#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cerrno>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::async_io::bsd_native::detail {

/**
 * Completion channel selected for receiver delivery.
 *
 * Per completion-semantics contract: set_value(ec, ...) is the universal
 * observable exit (success, cancel, recoverable failure); set_stopped is
 * reserved exclusively for io_context::stop() aborting inflight I/O.
 */
enum class kqueue_receiver_completion {
  /** set_value(empty ec, result, flags). */
  value,
  /** set_value(ec, result, flags) where ec carries a recoverable error. */
  value_with_ec,
  /** set_stopped — ONLY when io_context::stop() aborts this op. */
  stopped,
};

/** Translates kqueue context results into receiver completion signals. */
template <class Receiver>
class kqueue_receiver_operation : public kqueue_io_operation_base {
 public:
  kqueue_receiver_operation(const kqueue_receiver_operation&) = delete;
  kqueue_receiver_operation& operator=(const kqueue_receiver_operation&) =
      delete;
  kqueue_receiver_operation(kqueue_receiver_operation&&) = delete;
  kqueue_receiver_operation& operator=(kqueue_receiver_operation&&) = delete;
  ~kqueue_receiver_operation() noexcept override = default;

  /**
   * Delivers the selected completion signal to the receiver.
   *
   * The `value` branch preserves the `result < 0` guard mandated by
   * patch 02 §9.2: a kevent may report readiness, then perform_io()
   * returns a negative errno, but completion_ is still `value` because
   * the CQE/event handler only updates `result`, not `completion_`.
   * That errno must surface through set_value(ec, ...) rather than
   * being lost.
   */
  void execute() noexcept override {
    switch (completion_) {
      case kqueue_receiver_completion::value:
        if (this->result < 0) {
          bexec::set_value(
              std::move(receiver_),
              std::error_code(-this->result, std::generic_category()),
              this->result, this->flags);
        } else {
          bexec::set_value(std::move(receiver_), std::error_code{},
                           this->result, this->flags);
        }
        break;
      case kqueue_receiver_completion::value_with_ec:
        bexec::set_value(std::move(receiver_), error_, this->result,
                         this->flags);
        break;
      case kqueue_receiver_completion::stopped:
        bexec::set_stopped(std::move(receiver_));
        break;
    }
  }

  void complete_submit_error(int result_code) noexcept override {
    complete_with_ec(result_code);
  }

  void complete_submit_stopped() noexcept override { complete_with_stopped(); }

 protected:
  kqueue_receiver_operation(kqueue_context& context, Receiver receiver)
      : context_(&context), receiver_(std::move(receiver)) {}

  [[nodiscard]] bool stop_requested() const noexcept {
    auto environment = bexec::get_env(receiver_);
    auto token = bexec::query(environment, bexec::get_stop_token);
    return token.stop_requested();
  }

  void complete_with_value() noexcept {
    completion_ = kqueue_receiver_completion::value;
  }

  /**
   * Selects set_value(ec, ...) where ec carries a recoverable error
   * (user cancel via stop_token, or preparation/registration failure
   * reported through complete_submit_error).
   */
  void complete_with_ec(int result_code) noexcept {
    completion_ = kqueue_receiver_completion::value_with_ec;
    error_ = std::error_code(-result_code, std::generic_category());
  }

  /**
   * Selects set_stopped. Reached only via complete_submit_stopped(),
   * i.e. when io_context::stop() aborts this inflight I/O operation.
   */
  void complete_with_stopped() noexcept {
    completion_ = kqueue_receiver_completion::stopped;
  }

  /**
   * Starts a kqueue operation or posts an immediate completion.
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

  kqueue_context* context_;
  std::remove_cvref_t<Receiver> receiver_;
  kqueue_receiver_completion completion_ = kqueue_receiver_completion::value;
  /** Leading ec for the value_with_ec channel; empty for value. */
  std::error_code error_;
};

}  // namespace bnio::async_io::bsd_native::detail

#endif  // BNIO_ASYNC_IO_BSD_DETAIL_KQUEUE_RECEIVER_OPERATION_H_
