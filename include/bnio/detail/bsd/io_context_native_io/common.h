/**
 * @file common.h
 * @brief Common BSD native I/O operation support for io_context.
 *
 * Shared operation states and senders adapting kqueue request objects to
 * io_context. Included standalone it only pulls in <bnio/io_context.h>; the
 * real content is compiled by re-inclusion from
 * detail/posix/io_context/class.h after the io_context definition.
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
#include <bnio/detail/error_code.h>

#include <algorithm>
#include <cerrno>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::detail {

/** Returns whether the receiver's environment stop token has been requested. */
template <class Receiver>
[[nodiscard]] bool stop_requested(const Receiver& receiver) noexcept {
  auto environment = bexec::get_env(receiver);
  auto token = bexec::query(environment, bexec::get_stop_token);
  return token.stop_requested();
}

/** Converts a negative errno result into a generic-category error code. */
[[nodiscard]] inline std::error_code errno_result(int result) noexcept {
  return std::error_code(-result, std::generic_category());
}

/** Default eager-mode control: reads the context's immutable immediate-I/O
 *  switch, fixed at construction. */
class context_eager_control {
 public:
  explicit context_eager_control(io_context* context) noexcept
      : context_(context) {}
  [[nodiscard]] bool operator()() const noexcept {
    return context_->enable_immediate_io();
  }

 private:
  io_context* context_;
};

/**
 * Operation state adapting one objectized kqueue request to a downstream
 * receiver: start() probes once, then completes eagerly or waits for the
 * filter.
 */
template <io_context::schedule_kind Kind, class Request, class Control,
          class Receiver>
class native_io_operation : public io_context::operation_base {
 public:
  /** Binds the operation to its context, request, control, and receiver. */
  native_io_operation(io_context& context, Request request, Control control,
                      Receiver receiver)
      : context_(&context),
        request_(std::move(request)),
        control_(std::move(control)),
        receiver_(std::move(receiver)) {}

  /** Prepares the native registration for the run loop. */
  void prepare(async_io::bsd_native::kqueue_helper& helper) noexcept override {
    request_.prepare(helper);
  }

  /** Selects error completion when kqueue registration fails. */
  void complete_submit_error(int result) noexcept override {
    // kevent register failure: route through set_value(ec, ...).
    completion_ = completion_kind::value_with_ec;
    error_ = errno_result(result);
    this->result = result;
    this->flags = 0;
  }

  /** Marks the completion stopped when io_context::stop() aborts this
   *  inflight operation; execute() arbitrates the final channel. */
  void complete_submit_stopped() noexcept override {
    completion_ = completion_kind::stopped;
  }

  /** Returns true: the request performs the syscall after readiness. */
  [[nodiscard]] bool owns_io_step() const noexcept override { return true; }

  /** Performs one bounded nonblocking I/O step through the request. */
  [[nodiscard]] int perform_io() noexcept override {
    return request_.perform_io();
  }

  /** Probes the descriptor once, then completes or registers. */
  void start() noexcept {
    if (stop_requested(receiver_)) {
      // Token cancel at start: mark stopped; execute()'s arbitration
      // delivers set_stopped (token wins) or set_value(operation_canceled).
      completion_ = completion_kind::stopped;
      this->result = 0;
      this->flags = 0;
      publish_cpu_or_complete_inline();
      return;
    }

    // Without the eager probe, waitable requests park on -EAGAIN and let the
    // run loop register for readiness; files (and connect) still run their
    // first syscall inline.
    if constexpr (schedule_policy<Kind>::k_immediate) {
      this->result =
          control_() ? request_.start_io()
                     : async_io::bsd_native::detail::start_deferred(request_);
    } else {
      this->result = async_io::bsd_native::detail::start_deferred(request_);
    }
    this->flags = 0;
    if (async_io::bsd_native::detail::should_wait(request_, this->result)) {
      publish_io_or_complete_stopped();
    } else {
      // Immediate completion (success or non-EAGAIN errno).
      if (this->result < 0) {
        completion_ = completion_kind::value_with_ec;
        error_ = errno_result(this->result);
      } else {
        completion_ = completion_kind::value;
      }
      publish_cpu_or_complete_inline();
    }
  }

  /**
   * Delivers the completion.
   *
   * The stopped branch arbitrates: a cancelled stop token wins with
   * set_stopped(); otherwise set_value(operation_canceled, -1, 0). -1, not
   * 0, so a descriptor-yielding accept never hands out a descriptor
   * (byte-count requests clamp to 0 anyway).
   */
  void execute() noexcept override {
    switch (completion_) {
      case completion_kind::value:
        // Success or a negative errno from perform_io(); derive ec from
        // result so the errno surfaces through set_value(ec, ...).
        if (this->result < 0) {
          request_.set_value(std::move(receiver_), errno_result(this->result),
                             this->result, this->flags);
        } else {
          request_.set_value(std::move(receiver_),
                             bnio::detail::empty_error_code, this->result,
                             this->flags);
        }
        break;
      case completion_kind::value_with_ec:
        // errno / register failure: error_ carries ec.
        request_.set_value(std::move(receiver_), error_, this->result,
                           this->flags);
        break;
      case completion_kind::stopped:
        if (stop_requested(receiver_)) {
          bexec::set_stopped(std::move(receiver_));
        } else {
          request_.set_value(
              std::move(receiver_),
              std::make_error_code(std::errc::operation_canceled), -1, 0);
        }
        break;
    }
  }

 private:
  /** Publishes to the CPU queue; completes inline if the context already
   *  stopped and rejected the publish. */
  void publish_cpu_or_complete_inline() noexcept {
    if (!schedule_policy<Kind>::publish_cpu(*context_, *this)) {
      execute();
    }
  }

  /** Publishes the would-block result for passive registration; completes
   *  through the stop channel if the context is already stopping. */
  void publish_io_or_complete_stopped() noexcept {
    // Keep completion_ = value: a later successful perform_io() overwrites
    // result, and an eager value_with_ec would leak the stale EAGAIN.
    completion_ = completion_kind::value;
    if (!schedule_policy<Kind>::publish_io(*context_, *this)) {
      // Context already stopping: mark stopped and complete inline; the
      // token arbitration in execute() picks the final channel.
      complete_submit_stopped();
      execute();
    }
  }

  /** Completion channel staged by start(), the publish paths, and the
   *  abort hooks; execute() maps it to the final receiver signal. */
  enum class completion_kind {
    value,          // success, ec={}
    value_with_ec,  // errno / register failure -> set_value(ec, ...)
    stopped,  // abort or token-at-start marking; the final channel (stopped
              // vs value(operation_canceled)) is decided by execute()
  };

  io_context* context_;
  Request request_;
  Control control_;
  std::remove_cvref_t<Receiver> receiver_;
  completion_kind completion_ = completion_kind::value;
  std::error_code error_ = bnio::detail::empty_error_code;
};

/**
 * Sender for one objectized kqueue I/O request, forwarding the request's
 * completion signatures; Control overrides the default eager predicate.
 */
template <class Request, class Control = context_eager_control,
          io_context::schedule_kind Kind = io_context::schedule_kind::post>
class native_io_sender {
 public:
  /** Completion signatures forwarded from the request type. */
  using completion_signatures = typename Request::completion_signatures;

  /** Creates a sender bound to the context and request. */
  native_io_sender(io_context& context, Request request) noexcept
      : context_(&context),
        request_(std::move(request)),
        control_(context_eager_control{&context}) {}

  /** Creates a sender bound to the context and request, with an explicit
   *  eager-mode control. */
  native_io_sender(io_context& context, Request request,
                   Control control) noexcept
      : context_(&context),
        request_(std::move(request)),
        control_(std::move(control)) {}

  /** Connects the sender, moving the request and control. */
  template <class Receiver>
  auto connect(Receiver receiver) && {
    return native_io_operation<Kind, Request, Control,
                               std::remove_cvref_t<Receiver> >(
        *context_, std::move(request_), std::move(control_),
        std::move(receiver));
  }

  /** Connects the sender, copying the request and control. */
  template <class Receiver>
    requires std::copy_constructible<Request> &&
             std::copy_constructible<Control>
  auto connect(Receiver receiver) const& {
    return native_io_operation<Kind, Request, Control,
                               std::remove_cvref_t<Receiver> >(
        *context_, request_, control_, std::move(receiver));
  }

 private:
  io_context* context_;
  Request request_;
  Control control_;
};

/**
 * Poll operation on the kqueue backend; readiness comes from the fired filter,
 * so no data syscall is issued. Published straight to the I/O queues.
 */
template <io_context::schedule_kind Kind, class Receiver>
class native_poll_operation : public io_context::operation_base {
 public:
  /** Binds the operation to its context, descriptor, mask, and receiver. */
  native_poll_operation(io_context& context,
                        async_io::descriptor_view descriptor,
                        unsigned poll_mask, Receiver receiver)
      : context_(&context),
        request_(descriptor, poll_mask),
        receiver_(std::move(receiver)) {}

  /** Prepares the native registration for the run loop. */
  void prepare(async_io::bsd_native::kqueue_helper& helper) noexcept override {
    request_.prepare(helper);
  }

  /** Selects error completion when kqueue registration fails. */
  void complete_submit_error(int result) noexcept override {
    completion_ = completion_kind::value_with_ec;
    error_ = errno_result(result);
  }

  /** Marks the completion stopped when io_context::stop() aborts this
   *  inflight operation; execute() arbitrates the final channel. */
  void complete_submit_stopped() noexcept override {
    completion_ = completion_kind::stopped;
  }

  /** Checks the stop token, then registers for readiness. */
  void start() noexcept {
    if (stop_requested(receiver_)) {
      // Token cancel at start: mark stopped; execute()'s arbitration
      // delivers set_stopped (token wins) or set_value(operation_canceled).
      completion_ = completion_kind::stopped;
      if (!schedule_policy<Kind>::publish_cpu(*context_, *this)) {
        // Context already stopping: complete inline instead of stranding.
        execute();
      }
      return;
    }

    completion_ = completion_kind::value;
    if (!schedule_policy<Kind>::publish_io(*context_, *this)) {
      // Context already stopping: mark stopped and complete inline; the
      // token arbitration in execute() picks the final channel.
      complete_submit_stopped();
      execute();
    }
  }

  /**
   * Delivers the typed poll completion.
   *
   * The ready mask is meaningful only on success; on every other channel it
   * is 0U, with the error in the leading ec. A negative result is never cast
   * to unsigned (that would wrap and trip a caller's mask test).
   */
  void execute() noexcept override {
    switch (completion_) {
      case completion_kind::value:
        if (this->result < 0) {
          bexec::set_value(std::move(receiver_), errno_result(this->result),
                           0U);
        } else {
          bexec::set_value(std::move(receiver_), bnio::detail::empty_error_code,
                           static_cast<unsigned>(this->result));
        }
        break;
      case completion_kind::value_with_ec:
        bexec::set_value(std::move(receiver_), error_, 0U);
        break;
      case completion_kind::stopped:
        // Stopped: a cancelled stop token wins with set_stopped(); otherwise
        // the abort delivers set_value(operation_canceled, 0).
        if (stop_requested(receiver_)) {
          bexec::set_stopped(std::move(receiver_));
        } else {
          bexec::set_value(std::move(receiver_),
                           std::make_error_code(std::errc::operation_canceled),
                           0U);
        }
        break;
    }
  }

 private:
  /** Completion channel staged by start() and the abort hooks; execute()
   *  maps it to the final receiver signal. */
  enum class completion_kind {
    value,          // success, ec={}
    value_with_ec,  // errno / register failure -> set_value(ec, ...)
    stopped,  // abort or token-at-start marking; the final channel (stopped
              // vs value(operation_canceled)) is decided by execute()
  };

  io_context* context_;
  async_io::bsd_native::kqueue_poll_request request_;
  std::remove_cvref_t<Receiver> receiver_;
  completion_kind completion_ = completion_kind::value;
  std::error_code error_ = bnio::detail::empty_error_code;
};

/** Sender for descriptor polling on the kqueue backend. */
template <io_context::schedule_kind Kind = io_context::schedule_kind::post>
class native_poll_sender {
 public:
  /** Completes with the ready poll mask, or through the error, cancel, or
   *  stop channel. */
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, unsigned), bexec::set_stopped_t()>;

  /** Creates a sender bound to the context, descriptor, and poll mask. */
  native_poll_sender(io_context& context, async_io::descriptor_view descriptor,
                     unsigned poll_mask) noexcept
      : context_(&context), descriptor_(descriptor), poll_mask_(poll_mask) {}

  /** Connects the sender to a receiver, creating the poll operation state. */
  template <class Receiver>
  auto connect(Receiver receiver) const {
    return native_poll_operation<Kind, std::remove_cvref_t<Receiver> >(
        *context_, descriptor_, poll_mask_, std::move(receiver));
  }

 private:
  io_context* context_;
  async_io::descriptor_view descriptor_;
  unsigned poll_mask_;
};

/**
 * DNS resolve on the CPU queue: canceled token at start() gives set_stopped();
 * an already-stopped context gives set_value(operation_canceled, 0),
 * otherwise the resolver result (a later stop is not observed).
 */
template <io_context::schedule_kind Kind, class Receiver>
class resolve_operation : public async_io::bsd_native::kqueue_operation_base {
 public:
  /** Binds the operation to its context, query, and result storage. */
  resolve_operation(io_context& context, async_io::dns_query query,
                    async_io::dns_result_view result, Receiver receiver)
      : context_(&context),
        query_(std::move(query)),
        result_(result),
        receiver_(std::move(receiver)) {}

  /** Records a stop cancel, then publishes to the CPU queue. */
  void start() noexcept {
    // Token check at the start observation point: a cancel here is marked
    // and execute() delivers set_stopped for it.
    canceled_ = stop_requested(receiver_);
    if (!schedule_policy<Kind>::publish_cpu(*context_, *this)) {
      // Context already stopping: complete inline instead of stranding.
      execute();
    }
  }

  /** Delivers the three-way arbitration result described at class scope. */
  void execute() noexcept override {
    if (canceled_) {
      // Token cancel marked at start: the token wins -> set_stopped.
      bexec::set_stopped(std::move(receiver_));
      return;
    }

    if (context_->is_stopped()) {
      // Context aborted before resolve ran: skip DNS and deliver
      // set_value(operation_canceled, 0).
      bexec::set_value(std::move(receiver_),
                       std::make_error_code(std::errc::operation_canceled),
                       std::size_t{0});
      return;
    }

    std::size_t count = 0;
    const std::error_code ec = async_io::resolve_dns(query_, result_, count);
    // Success or resolver error both exit through set_value(ec, count); a
    // stop after the is_stopped() check above is not observed here.
    bexec::set_value(std::move(receiver_), ec, count);
  }

 private:
  io_context* context_;
  async_io::dns_query query_;
  async_io::dns_result_view result_;
  std::remove_cvref_t<Receiver> receiver_;
  bool canceled_ = false;
};

/** Sender for DNS resolution on the context CPU queue. */
template <io_context::schedule_kind Kind = io_context::schedule_kind::post>
class resolve_sender {
 public:
  /** Completes with the resolved endpoint count, or through the error,
   *  cancel, or stop channel. */
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  /** Creates a sender bound to the context, query, and result storage. */
  resolve_sender(io_context& context, async_io::dns_query query,
                 async_io::dns_result_view result)
      : context_(&context), query_(std::move(query)), result_(result) {}

  /** Connects the sender, moving the query into the operation state. */
  template <class Receiver>
  auto connect(Receiver receiver) && {
    return resolve_operation<Kind, std::remove_cvref_t<Receiver> >(
        *context_, std::move(query_), result_, std::move(receiver));
  }

  /** Connects the sender, copying the query into the operation state. */
  template <class Receiver>
  auto connect(Receiver receiver) const& {
    return resolve_operation<Kind, std::remove_cvref_t<Receiver> >(
        *context_, query_, result_, std::move(receiver));
  }

 private:
  io_context* context_;
  async_io::dns_query query_;
  async_io::dns_result_view result_;
};

/** Creates a native I/O sender honoring the schedule kind. */
template <io_context::schedule_kind Kind, class Request,
          class Control = context_eager_control>
[[nodiscard]] auto make_io_sender(io_context& context,
                                  Request request) noexcept {
  return native_io_sender<Request, Control, Kind>(context, std::move(request));
}

template <io_context::schedule_kind Kind, class Request, class Control>
[[nodiscard]] auto make_io_sender(io_context& context, Request request,
                                  Control control) noexcept {
  return native_io_sender<Request, Control, Kind>(context, std::move(request),
                                                  std::move(control));
}

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_BSD_IO_CONTEXT_NATIVE_IO_COMMON_H_
