/**
 * @file io_request.h
 * @brief Internal kqueue I/O request types.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_DETAIL_IO_REQUEST_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_DETAIL_IO_REQUEST_H_

#include <bnio/async_io/bsd/kqueue_context_base.h>
#include <bnio/detail/error_code.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cerrno>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::async_io::bsd_native::detail {

/** Returns whether the receiver's associated stop token is canceled. */
template <class Receiver>
[[nodiscard]] bool stop_requested(const Receiver& receiver) noexcept {
  auto environment = bexec::get_env(receiver);
  auto token = bexec::query(environment, bexec::get_stop_token);
  return token.stop_requested();
}

/** Returns whether the result is a transient would-block (-EAGAIN or
 *  -EWOULDBLOCK) requiring another readiness round. */
[[nodiscard]] inline bool should_wait(int result) noexcept {
  return result == -EAGAIN || result == -EWOULDBLOCK;
}

/** Returns whether the operation must wait after @p result, preferring the
 *  request's own should_wait() when it provides one. */
template <class Request>
[[nodiscard]] bool should_wait(Request& request, int result) noexcept {
  if constexpr (requires { request.should_wait(result); }) {
    return request.should_wait(result);
  } else {
    return should_wait(result);
  }
}

/** Concept that detects whether a request can perform an immediate nonblocking
 *  I/O call before registering with kqueue for readiness. */
template <class Request>
concept has_start_io = requires(Request& req) {
  { req.start_io() } -> std::convertible_to<int>;
};

/** Concept that detects whether a request can skip its first native I/O call
 *  and park on readiness instead. */
template <class Request>
concept has_start_deferred = requires(Request& req) {
  { req.start_deferred() } -> std::convertible_to<int>;
};

/** Starts a request without the eager probe: deferrable requests return the
 *  would-block sentinel and wait for readiness, everything else runs
 *  perform_io() inline (regular files never become ready on kqueue, and
 *  connect must be issued before EVFILT_WRITE can fire). */
template <class Request>
[[nodiscard]] int start_deferred(Request& request) noexcept {
  if constexpr (has_start_deferred<Request>) {
    return request.start_deferred();
  } else {
    return request.perform_io();
  }
}

/** Operation for a request whose native call is attempted before readiness.
 *
 * `stopped_` marks the stop channel (abort or start-time token cancel);
 * execute() arbitrates it to set_stopped or operation_canceled. A negative
 * `result` surfaces through set_value(ec, ...).
 */
template <class Request, class Receiver>
class kqueue_ready_io_operation : public kqueue_io_operation_base {
 public:
  /** Creates the operation bound to the context, request, and receiver. */
  kqueue_ready_io_operation(kqueue_context& context, Request request,
                            Receiver receiver)
      : context_(&context),
        request_(std::move(request)),
        receiver_(std::move(receiver)) {}

  /** Disabled: operations are queued intrusively. */
  kqueue_ready_io_operation(const kqueue_ready_io_operation&) = delete;

  /** Disabled: operations are queued intrusively. */
  kqueue_ready_io_operation& operator=(const kqueue_ready_io_operation&) =
      delete;

  /** Disabled: operations are queued intrusively. */
  kqueue_ready_io_operation(kqueue_ready_io_operation&&) = delete;

  /** Disabled: operations are queued intrusively. */
  kqueue_ready_io_operation& operator=(kqueue_ready_io_operation&&) = delete;

  /** Delegates native registration preparation to the request. */
  void prepare(kqueue_helper& helper) noexcept override {
    request_.prepare(helper);
  }

  /** Records the submission failure errno as the completion result. */
  void complete_submit_error(int result_code) noexcept override {
    result = result_code;
  }

  /** Selects the stop channel for an aborting io_context::stop(). */
  void complete_submit_stopped() noexcept override { stopped_ = true; }

  /** Returns true: this operation performs the native I/O itself. */
  [[nodiscard]] bool owns_io_step() const noexcept override { return true; }

  /** Performs one bounded nonblocking I/O call through the request. */
  [[nodiscard]] int perform_io() noexcept override {
    return request_.perform_io();
  }

  /** Starts: checks the stop token, tries an immediate nonblocking I/O step,
   *  and otherwise publishes for passive preparation. */
  void start() noexcept {
    if (detail::stop_requested(receiver_)) {
      stopped_ = true;
      (void)context_->post(*this);
      return;
    }

    if (try_complete_immediate()) {
      return;
    }

    context_->publish_io(*this);
  }

  /**
   * Delivers the completion: `stopped_` yields set_stopped or
   * set_value(operation_canceled, -1, 0), otherwise the real result.
   * -1, not 0, because accept forwards `result` verbatim as a descriptor.
   */
  void execute() noexcept override {
    if (stopped_) {
      if (detail::stop_requested(receiver_)) {
        bexec::set_stopped(std::move(receiver_));
      } else {
        request_.set_value(std::move(receiver_),
                           std::make_error_code(std::errc::operation_canceled),
                           -1, 0);
      }
      return;
    }
    std::error_code ec = bnio::detail::empty_error_code;
    if (result < 0) {
      ec = std::error_code(-result, std::generic_category());
    }
    request_.set_value(std::move(receiver_), ec, result, flags);
  }

 private:
  /** Attempts one nonblocking call before readiness registration; returns
   *  true when start_io() did not return EAGAIN/EWOULDBLOCK and the
   *  operation was posted inline, false when the caller must register it. */
  [[nodiscard]] bool try_complete_immediate() noexcept {
    if constexpr (has_start_io<Request>) {
      result = request_.start_io();
      flags = 0;
      if (detail::should_wait(request_, result)) {
        return false;
      }
      (void)context_->post(*this);
      return true;
    } else {
      return false;
    }
  }

  /** Context whose run loop drives this operation. */
  kqueue_context* context_;

  /** Request that prepares the registration, performs the native I/O step,
   *  and translates the completion into set_value arguments. */
  Request request_;

  /** Receiver completed by this operation. */
  std::remove_cvref_t<Receiver> receiver_;

  /** Marks the stop channel; execute() arbitrates the final signal. */
  bool stopped_ = false;
};

/** Sender for a readiness-backed nonblocking request. */
template <class Request>
class kqueue_ready_io_sender {
 public:
  /** Completion signatures forwarded from the request type. */
  using completion_signatures = typename Request::completion_signatures;

  /** Creates a sender bound to the context and request. */
  kqueue_ready_io_sender(kqueue_context& context, Request request) noexcept
      : context_(&context), request_(std::move(request)) {}

  /** Connects to a receiver, moving the request into the operation. */
  template <class Receiver>
  auto connect(Receiver receiver) && {
    return kqueue_ready_io_operation<Request, std::remove_cvref_t<Receiver>>(
        *context_, std::move(request_), std::move(receiver));
  }

  /** Connects to a receiver, copying the request into the operation. */
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
