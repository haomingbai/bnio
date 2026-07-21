/**
 * @file io_request.h
 * @brief Internal kqueue I/O request types.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_DETAIL_IO_REQUEST_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_DETAIL_IO_REQUEST_H_

#include <bnio/async_io/bsd/kqueue_context_base.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cerrno>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::async_io::bsd_native::detail {

template <class Receiver>
[[nodiscard]] bool stop_requested(const Receiver& receiver) noexcept {
  auto environment = bexec::get_env(receiver);
  auto token = bexec::query(environment, bexec::get_stop_token);
  return token.stop_requested();
}

[[nodiscard]] inline bool should_wait(int result) noexcept {
  return result == -EAGAIN || result == -EWOULDBLOCK;
}

template <class Request>
[[nodiscard]] bool should_wait(Request& request, int result) noexcept {
  if constexpr (requires { request.should_wait(result); }) {
    return request.should_wait(result);
  } else {
    return should_wait(result);
  }
}

template <class Receiver>
void complete_error(Receiver&& receiver, int result) noexcept {
  bexec::set_error(std::forward<Receiver>(receiver),
                   std::error_code(-result, std::generic_category()));
}

/** Operation for a request whose native call is attempted before readiness. */
template <class Request, class Receiver>
class kqueue_ready_io_operation : public kqueue_io_operation_base {
 public:
  kqueue_ready_io_operation(kqueue_context& context, Request request,
                            Receiver receiver)
      : context_(&context),
        request_(std::move(request)),
        receiver_(std::move(receiver)) {}

  kqueue_ready_io_operation(const kqueue_ready_io_operation&) = delete;
  kqueue_ready_io_operation& operator=(const kqueue_ready_io_operation&) =
      delete;
  kqueue_ready_io_operation(kqueue_ready_io_operation&&) = delete;
  kqueue_ready_io_operation& operator=(kqueue_ready_io_operation&&) = delete;

  void prepare(kqueue_helper& helper) noexcept override {
    request_.prepare(helper);
  }

  void complete_submit_error(int result_code) noexcept override {
    result = result_code;
  }

  [[nodiscard]] bool owns_io_step() const noexcept override { return true; }

  [[nodiscard]] int perform_io() noexcept override {
    return request_.perform_io();
  }

  void start() noexcept {
    if (detail::stop_requested(receiver_)) {
      stopped_ = true;
      (void)context_->post(*this);
      return;
    }

    result = request_.start_io();
    flags = 0;
    if (detail::should_wait(request_, result)) {
      context_->publish_io(*this);
    } else {
      (void)context_->post(*this);
    }
  }

  void execute() noexcept override {
    if (stopped_) {
      bexec::set_stopped(std::move(receiver_));
    } else if (result < 0) {
      complete_error(std::move(receiver_), result);
    } else {
      request_.set_value(std::move(receiver_), result, flags);
    }
  }

 private:
  kqueue_context* context_;
  Request request_;
  std::remove_cvref_t<Receiver> receiver_;
  bool stopped_ = false;
};

/** Sender for a readiness-backed nonblocking request. */
template <class Request>
class kqueue_ready_io_sender {
 public:
  using completion_signatures = typename Request::completion_signatures;

  kqueue_ready_io_sender(kqueue_context& context, Request request) noexcept
      : context_(&context), request_(std::move(request)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return kqueue_ready_io_operation<Request, std::remove_cvref_t<Receiver>>(
        *context_, std::move(request_), std::move(receiver));
  }

  template <class Receiver>
    requires std::copy_constructible<Request>
  auto connect(Receiver receiver) const& {
    return kqueue_ready_io_operation<Request, std::remove_cvref_t<Receiver>>(
        *context_, request_, std::move(receiver));
  }

 private:
  kqueue_context* context_;
  Request request_;
};

}  // namespace bnio::async_io::bsd_native::detail

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_DETAIL_IO_REQUEST_H_
