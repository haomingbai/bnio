#pragma once
#ifndef BUPP_ASYNC_IO_BSD_KQUEUE_OPERATIONS_POLL_H_
#define BUPP_ASYNC_IO_BSD_KQUEUE_OPERATIONS_POLL_H_

#include <bupp/async_io/bsd/detail/kqueue_receiver_operation.h>
#include <bupp/async_io/descriptor_view.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bupp::async_io::bsd_native {

/** Prepared kqueue poll request reusable by higher abstraction layers. */
class kqueue_poll_request {
 public:
  kqueue_poll_request(descriptor_view descriptor, unsigned poll_mask) noexcept
      : descriptor_(descriptor), poll_mask_(poll_mask) {}

  void prepare(kqueue_helper& helper) const noexcept {
    helper.prep_poll_add(descriptor_.native_handle(), poll_mask_);
  }

 private:
  descriptor_view descriptor_;
  unsigned poll_mask_;
};

/** Raw poll operation that reports result and native flags. */
template <class Receiver>
class kqueue_poll_operation
    : public detail::kqueue_receiver_operation<Receiver> {
 public:
  kqueue_poll_operation(kqueue_context& context, descriptor_view descriptor,
                        unsigned poll_mask, Receiver receiver)
      : detail::kqueue_receiver_operation<Receiver>(context,
                                                    std::move(receiver)),
        request_(descriptor, poll_mask) {}

  void prepare(kqueue_helper& helper) noexcept { request_.prepare(helper); }

  void start() noexcept { this->start_io(*this); }

 private:
  kqueue_poll_request request_;
};

/** Operation state used by the typed kqueue poll sender. */
template <class Receiver>
class kqueue_poll_sender_operation : public kqueue_operation_base {
 public:
  kqueue_poll_sender_operation(kqueue_context& context,
                               descriptor_view descriptor, unsigned poll_mask,
                               Receiver receiver)
      : context_(&context),
        request_(descriptor, poll_mask),
        receiver_(std::move(receiver)) {}

  kqueue_poll_sender_operation(const kqueue_poll_sender_operation&) = delete;
  kqueue_poll_sender_operation& operator=(const kqueue_poll_sender_operation&) =
      delete;
  kqueue_poll_sender_operation(kqueue_poll_sender_operation&&) = delete;
  kqueue_poll_sender_operation& operator=(kqueue_poll_sender_operation&&) =
      delete;

  void prepare(kqueue_helper& helper) noexcept { request_.prepare(helper); }

  void start() noexcept {
    if (stop_requested()) {
      completion_ = completion_kind::stopped;
      (void)context_->post(*this);
      return;
    }

    const int submit_result = context_->submit(*this);
    if (submit_result < 0) {
      completion_ = completion_kind::error;
      error_ = std::error_code(-submit_result, std::generic_category());
      (void)context_->post(*this);
    }
  }

  void execute() noexcept override {
    switch (completion_) {
      case completion_kind::value:
        if (result < 0) {
          bexec::set_error(std::move(receiver_),
                           std::error_code(-result, std::generic_category()));
        } else {
          bexec::set_value(std::move(receiver_), static_cast<unsigned>(result));
        }
        break;
      case completion_kind::error:
        bexec::set_error(std::move(receiver_), error_);
        break;
      case completion_kind::stopped:
        bexec::set_stopped(std::move(receiver_));
        break;
    }
  }

 private:
  enum class completion_kind {
    value,
    error,
    stopped,
  };

  [[nodiscard]] bool stop_requested() const noexcept {
    auto environment = bexec::get_env(receiver_);
    auto token = bexec::query(environment, bexec::get_stop_token);
    return token.stop_requested();
  }

  kqueue_context* context_;
  kqueue_poll_request request_;
  std::remove_cvref_t<Receiver> receiver_;
  completion_kind completion_ = completion_kind::value;
  std::error_code error_;
};

/** Sender returned by kqueue_context::async_poll. */
class kqueue_poll_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(unsigned),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  kqueue_poll_sender(kqueue_context& context, descriptor_view descriptor,
                     unsigned poll_mask) noexcept
      : context_(&context), descriptor_(descriptor), poll_mask_(poll_mask) {}

  template <class Receiver>
  auto connect(Receiver receiver) const {
    return kqueue_poll_sender_operation<std::remove_cvref_t<Receiver>>(
        *context_, descriptor_, poll_mask_, std::move(receiver));
  }

 private:
  kqueue_context* context_;
  descriptor_view descriptor_;
  unsigned poll_mask_;
};

/** @cond BUPP_DETAIL */

inline auto kqueue_context::async_poll(descriptor_view descriptor,
                                       unsigned poll_mask) {
  return kqueue_poll_sender(*this, descriptor, poll_mask);
}

/** @endcond */

}  // namespace bupp::async_io::bsd_native

#endif  // BUPP_ASYNC_IO_BSD_KQUEUE_OPERATIONS_POLL_H_
