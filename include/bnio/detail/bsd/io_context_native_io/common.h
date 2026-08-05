/**
 * @file common.h
 * @brief Common BSD native I/O operation support.
 */

#ifndef BNIO_DETAIL_BSD_IO_CONTEXT_NATIVE_IO_COMMON_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_BSD_IO_CONTEXT_NATIVE_IO_COMMON_H_

#include <bnio/async_io/bsd/kqueue_operations/file.h>
#include <bnio/async_io/bsd/kqueue_operations/poll.h>
#include <bnio/async_io/bsd/kqueue_operations/socket.h>
#include <bnio/async_io/dns/resolve.h>

#include <algorithm>
#include <cerrno>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::detail {

template <class Receiver>
[[nodiscard]] bool stop_requested(const Receiver& receiver) noexcept {
  auto environment = bexec::get_env(receiver);
  auto token = bexec::query(environment, bexec::get_stop_token);
  return token.stop_requested();
}

[[nodiscard]] inline std::error_code errno_result(int result) noexcept {
  return std::error_code(-result, std::generic_category());
}

template <class Request, class Receiver, bool EnableImmediate = true>
class native_io_operation : public io_context::operation_base {
 public:
  native_io_operation(io_context& context, Request request, Receiver receiver)
      : context_(&context),
        request_(std::move(request)),
        receiver_(std::move(receiver)) {}

  void prepare(async_io::bsd_native::kqueue_helper& helper) noexcept override {
    request_.prepare(helper);
  }

  void complete_submit_error(int result) noexcept override {
    // kevent register failure: route through set_value(ec, ...).
    completion_ = completion_kind::value_with_ec;
    error_ = errno_result(result);
    this->result = result;
    this->flags = 0;
  }

  void complete_submit_stopped() noexcept override {
    // io_context::stop() -> abort_inflight_io: the only set_stopped entry.
    completion_ = completion_kind::stopped;
  }

  [[nodiscard]] bool owns_io_step() const noexcept override { return true; }

  [[nodiscard]] int perform_io() noexcept override {
    return request_.perform_io();
  }

  void start() noexcept {
    if (stop_requested(receiver_)) {
      // stop-token cancel: route through set_value(operation_canceled, ...).
      completion_ = completion_kind::value_with_ec;
      error_ = std::make_error_code(std::errc::operation_canceled);
      this->result = 0;
      this->flags = 0;
      if (!context_->publish_cpu(*this)) {
        // Context already stopping: complete inline instead of stranding.
        execute();
      }
      return;
    }

    if constexpr (EnableImmediate) {
      this->result = request_.start_io();
    } else {
      this->result = request_.perform_io();
    }
    this->flags = 0;
    if (async_io::bsd_native::detail::should_wait(request_, this->result)) {
      // EAGAIN/EWOULDBLOCK: register with kqueue for readiness. Leave
      // completion_ as `value`; the kevent path will update this->result via
      // perform_io(), and execute()'s value branch re-derives ec from result
      // (§9.2 guard). Eagerly setting value_with_ec here would leak a stale
      // EAGAIN ec when the eventual perform_io() succeeds.
      completion_ = completion_kind::value;
      if (!context_->publish_io(*this)) {
        // Context already stopping: complete inline with stopped (the same
        // completion abort_inflight_io would deliver) instead of publishing
        // into a context that is shutting down.
        complete_submit_stopped();
        execute();
      }
    } else {
      // Immediate completion (success or non-EAGAIN errno).
      if (this->result < 0) {
        completion_ = completion_kind::value_with_ec;
        error_ = errno_result(this->result);
      } else {
        completion_ = completion_kind::value;
      }
      if (!context_->publish_cpu(*this)) {
        // Context already stopping: complete inline instead of stranding.
        execute();
      }
    }
  }

  void execute() noexcept override {
    switch (completion_) {
      case completion_kind::value:
        // Success, or a kevent-reported I/O step whose perform_io() returned
        // a negative errno (spurious wakeup / rearm failure). Re-derive ec
        // from result so the errno surfaces via set_value(ec, ...) rather
        // than being lost (§9.2 guard).
        if (this->result < 0) {
          request_.set_value(std::move(receiver_), errno_result(this->result),
                             this->result, this->flags);
        } else {
          request_.set_value(std::move(receiver_), std::error_code{},
                             this->result, this->flags);
        }
        break;
      case completion_kind::value_with_ec:
        // errno / register failure / stop-token cancel: error_ carries ec.
        request_.set_value(std::move(receiver_), error_, this->result,
                           this->flags);
        break;
      case completion_kind::stopped:
        // Sole source: io_context::stop() -> abort_inflight_io.
        bexec::set_stopped(std::move(receiver_));
        break;
    }
  }

 private:
  enum class completion_kind {
    value,          // success, ec={}
    value_with_ec,  // errno / cancel / register failure -> set_value(ec, ...)
    stopped,        // only io_context::stop()
  };

  io_context* context_;
  Request request_;
  std::remove_cvref_t<Receiver> receiver_;
  completion_kind completion_ = completion_kind::value;
  std::error_code error_;
};

template <class Request, bool EnableImmediate = true>
class native_io_sender {
 public:
  using completion_signatures = typename Request::completion_signatures;

  native_io_sender(io_context& context, Request request) noexcept
      : context_(&context), request_(std::move(request)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return native_io_operation<Request, std::remove_cvref_t<Receiver>,
                               EnableImmediate>(*context_, std::move(request_),
                                                std::move(receiver));
  }

  template <class Receiver>
    requires std::copy_constructible<Request>
  auto connect(Receiver receiver) const& {
    return native_io_operation<Request, std::remove_cvref_t<Receiver>,
                               EnableImmediate>(*context_, request_,
                                                std::move(receiver));
  }

 private:
  io_context* context_;
  Request request_;
};

template <class Receiver>
class native_poll_operation : public io_context::operation_base {
 public:
  native_poll_operation(io_context& context,
                        async_io::descriptor_view descriptor,
                        unsigned poll_mask, Receiver receiver)
      : context_(&context),
        request_(descriptor, poll_mask),
        receiver_(std::move(receiver)) {}

  void prepare(async_io::bsd_native::kqueue_helper& helper) noexcept override {
    request_.prepare(helper);
  }

  void complete_submit_error(int result) noexcept override {
    completion_ = completion_kind::value_with_ec;
    error_ = errno_result(result);
  }

  void complete_submit_stopped() noexcept override {
    // io_context::stop() -> abort_inflight_io: the only set_stopped entry.
    completion_ = completion_kind::stopped;
  }

  void start() noexcept {
    if (stop_requested(receiver_)) {
      // stop-token cancel: set_value(operation_canceled, ...).
      completion_ = completion_kind::value_with_ec;
      error_ = std::make_error_code(std::errc::operation_canceled);
      if (!context_->publish_cpu(*this)) {
        // Context already stopping: complete inline instead of stranding.
        execute();
      }
      return;
    }

    completion_ = completion_kind::value;
    if (!context_->publish_io(*this)) {
      // Context already stopping: complete inline with stopped (the same
      // completion abort_inflight_io would deliver) instead of publishing
      // into a context that is shutting down.
      complete_submit_stopped();
      execute();
    }
  }

  void execute() noexcept override {
    switch (completion_) {
      case completion_kind::value:
        if (this->result < 0) {
          bexec::set_value(std::move(receiver_), errno_result(this->result),
                           static_cast<unsigned>(this->result));
        } else {
          bexec::set_value(std::move(receiver_), std::error_code{},
                           static_cast<unsigned>(this->result));
        }
        break;
      case completion_kind::value_with_ec:
        bexec::set_value(std::move(receiver_), error_,
                         static_cast<unsigned>(this->result));
        break;
      case completion_kind::stopped:
        bexec::set_stopped(std::move(receiver_));
        break;
    }
  }

 private:
  enum class completion_kind {
    value,          // success, ec={}
    value_with_ec,  // errno / cancel / register failure -> set_value(ec, ...)
    stopped,        // only io_context::stop()
  };

  io_context* context_;
  async_io::bsd_native::kqueue_poll_request request_;
  std::remove_cvref_t<Receiver> receiver_;
  completion_kind completion_ = completion_kind::value;
  std::error_code error_;
};

class native_poll_sender {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, unsigned), bexec::set_stopped_t()>;

  native_poll_sender(io_context& context, async_io::descriptor_view descriptor,
                     unsigned poll_mask) noexcept
      : context_(&context), descriptor_(descriptor), poll_mask_(poll_mask) {}

  template <class Receiver>
  auto connect(Receiver receiver) const {
    return native_poll_operation<std::remove_cvref_t<Receiver> >(
        *context_, descriptor_, poll_mask_, std::move(receiver));
  }

 private:
  io_context* context_;
  async_io::descriptor_view descriptor_;
  unsigned poll_mask_;
};

template <class Receiver>
class resolve_operation : public async_io::bsd_native::kqueue_operation_base {
 public:
  resolve_operation(io_context& context, async_io::dns_query query,
                    async_io::dns_result_view result, Receiver receiver)
      : context_(&context),
        query_(std::move(query)),
        result_(result),
        receiver_(std::move(receiver)) {}

  void start() noexcept {
    canceled_ = stop_requested(receiver_);
    if (!context_->publish_cpu(*this)) {
      // Context already stopping: complete inline instead of stranding.
      execute();
    }
  }

  void execute() noexcept override {
    if (canceled_) {
      // stop-token cancel: set_value(operation_canceled, 0).
      bexec::set_value(std::move(receiver_),
                       std::make_error_code(std::errc::operation_canceled),
                       std::size_t{0});
      return;
    }

    std::size_t count = 0;
    const std::error_code ec = async_io::resolve_dns(query_, result_, count);
    // Success ec={}; failure ec carries the resolver error. Both exit through
    // set_value(ec, count). resolve runs on the CPU queue, so io_context::stop
    // never reaches it and set_stopped is not produced here.
    bexec::set_value(std::move(receiver_), ec, count);
  }

 private:
  io_context* context_;
  async_io::dns_query query_;
  async_io::dns_result_view result_;
  std::remove_cvref_t<Receiver> receiver_;
  bool canceled_ = false;
};

class resolve_sender {
 public:
  using completion_signatures = bexec::completion_signatures<bexec::set_value_t(
      std::error_code, std::size_t)>;

  resolve_sender(io_context& context, async_io::dns_query query,
                 async_io::dns_result_view result)
      : context_(&context), query_(std::move(query)), result_(result) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return resolve_operation<std::remove_cvref_t<Receiver> >(
        *context_, std::move(query_), result_, std::move(receiver));
  }

  template <class Receiver>
  auto connect(Receiver receiver) const& {
    return resolve_operation<std::remove_cvref_t<Receiver> >(
        *context_, query_, result_, std::move(receiver));
  }

 private:
  io_context* context_;
  async_io::dns_query query_;
  async_io::dns_result_view result_;
};

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_BSD_IO_CONTEXT_NATIVE_IO_COMMON_H_
