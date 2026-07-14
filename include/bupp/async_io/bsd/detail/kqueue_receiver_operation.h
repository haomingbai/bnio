#pragma once
#ifndef BUPP_ASYNC_IO_BSD_DETAIL_KQUEUE_RECEIVER_OPERATION_H_
#define BUPP_ASYNC_IO_BSD_DETAIL_KQUEUE_RECEIVER_OPERATION_H_

#include <bupp/async_io/bsd/kqueue_context_base.h>

#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bupp::async_io::bsd_native::detail {

enum class kqueue_receiver_completion {
  value,
  error,
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

  void execute() noexcept override {
    switch (completion_) {
      case kqueue_receiver_completion::value:
        if (this->result < 0) {
          bexec::set_error(
              std::move(receiver_),
              std::error_code(-this->result, std::generic_category()));
        } else {
          bexec::set_value(std::move(receiver_), this->result, this->flags);
        }
        break;
      case kqueue_receiver_completion::error:
        bexec::set_error(std::move(receiver_), error_);
        break;
      case kqueue_receiver_completion::stopped:
        bexec::set_stopped(std::move(receiver_));
        break;
    }
  }

  void complete_submit_error(int result_code) noexcept override {
    complete_with_error(result_code);
  }

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

  void complete_with_error(int result_code) noexcept {
    completion_ = kqueue_receiver_completion::error;
    error_ = std::error_code(-result_code, std::generic_category());
  }

  void complete_with_stopped() noexcept {
    completion_ = kqueue_receiver_completion::stopped;
  }

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

  kqueue_context* context_;
  std::remove_cvref_t<Receiver> receiver_;
  kqueue_receiver_completion completion_ = kqueue_receiver_completion::value;
  std::error_code error_;
};

}  // namespace bupp::async_io::bsd_native::detail

#endif  // BUPP_ASYNC_IO_BSD_DETAIL_KQUEUE_RECEIVER_OPERATION_H_
