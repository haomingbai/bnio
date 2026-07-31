/**
 * @file poll.h
 * @brief io_uring poll operations.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_POLL_H_
#define BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_POLL_H_

#include <bnio/async_io/descriptor_view.h>
#include <bnio/async_io/linux/detail/io_uring_receiver_operation.h>
#include <bnio/base/linux/submission_queue_entry.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cerrno>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::async_io::linux_native {

/**
 * Prepared io_uring poll request reusable by higher abstraction layers.
 */
class io_uring_poll_request {
 public:
  /**
   * Creates a poll request for a file descriptor and event mask.
   */
  io_uring_poll_request(descriptor_view descriptor, unsigned poll_mask) noexcept
      : descriptor_(descriptor), poll_mask_(poll_mask) {}

  /**
   * Prepares the poll SQE through the base-layer wrapper.
   *
   * @see io_uring_prep_poll_add
   */
  void prepare(bnio::base::submission_queue_entry& sqe) const noexcept {
    sqe.prep_poll_add(descriptor_.native_handle(), poll_mask_);
  }

 private:
  descriptor_view descriptor_;
  unsigned poll_mask_;
};

/**
 * Operation representing an io_uring poll request.
 */
template <class Receiver>
class io_uring_poll_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a poll operation for a file descriptor.
   */
  io_uring_poll_operation(io_uring_context& context, descriptor_view descriptor,
                          unsigned poll_mask, Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        request_(descriptor, poll_mask) {}

  /**
   * Prepares the poll SQE.
   *
   * @see io_uring_prep_poll_add
   */
  void prepare(bnio::base::submission_queue_entry& sqe) noexcept override {
    request_.prepare(sqe);
  }

  /**
   * Starts the poll operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  io_uring_poll_request request_;
};

/**
 * Operation state used by the typed io_uring poll sender.
 */
template <class Receiver>
class io_uring_poll_sender_operation : public io_uring_io_operation_base {
 public:
  /**
   * Creates a typed poll operation for a context and receiver.
   */
  io_uring_poll_sender_operation(io_uring_context& context,
                                 descriptor_view descriptor, unsigned poll_mask,
                                 Receiver receiver)
      : context_(&context),
        request_(descriptor, poll_mask),
        receiver_(std::move(receiver)) {}

  io_uring_poll_sender_operation(const io_uring_poll_sender_operation&) =
      delete;
  io_uring_poll_sender_operation& operator=(
      const io_uring_poll_sender_operation&) = delete;
  io_uring_poll_sender_operation(io_uring_poll_sender_operation&&) = delete;
  io_uring_poll_sender_operation& operator=(io_uring_poll_sender_operation&&) =
      delete;

  /**
   * Prepares the poll request.
   */
  void prepare(bnio::base::submission_queue_entry& sqe) noexcept override {
    request_.prepare(sqe);
  }

  /**
   * Starts the poll or posts an immediate canceled/stopped completion.
   *
   * A user-requested cancel (stop_token) completes through
   * set_value(operation_canceled, ...) rather than set_stopped; the
   * latter is reserved for io_context::stop().
   */
  void start() noexcept {
    if (stop_requested()) {
      completion_ = completion_kind::value_with_ec;
      error_ = std::error_code(ECANCELED, std::generic_category());
      (void)context_->post(*this);
      return;
    }

    completion_ = completion_kind::value;
    context_->publish_io(*this);
  }

  void complete_submit_error(int result) noexcept override {
    completion_ = completion_kind::value_with_ec;
    error_ = std::error_code(-result, std::generic_category());
  }

  void complete_submit_stopped() noexcept override {
    completion_ = completion_kind::stopped;
  }

  /**
   * Delivers the typed poll completion.
   *
   * The `value` branch preserves the `result < 0` guard: a poll CQE may
   * report a negative errno (e.g. EBADF) while completion_ is still
   * `value`; that errno must surface through set_value(ec, ...) rather
   * than being lost.
   */
  void execute() noexcept override {
    switch (completion_) {
      case completion_kind::value:
        if (result < 0) {
          bexec::set_value(std::move(receiver_),
                           std::error_code(-result, std::generic_category()),
                           static_cast<unsigned>(result));
        } else {
          bexec::set_value(std::move(receiver_), std::error_code{},
                           static_cast<unsigned>(result));
        }
        break;
      case completion_kind::value_with_ec:
        bexec::set_value(std::move(receiver_), error_,
                         static_cast<unsigned>(result));
        break;
      case completion_kind::stopped:
        bexec::set_stopped(std::move(receiver_));
        break;
    }
  }

 private:
  enum class completion_kind {
    value,
    value_with_ec,
    stopped,
  };

  [[nodiscard]] bool stop_requested() const noexcept {
    auto env = bexec::get_env(receiver_);
    auto token = bexec::query(env, bexec::get_stop_token);
    return token.stop_requested();
  }

  io_uring_context* context_;
  io_uring_poll_request request_;
  std::remove_cvref_t<Receiver> receiver_;
  completion_kind completion_ = completion_kind::value;
  std::error_code error_;
};

/**
 * Sender returned by io_uring_context poll APIs.
 */
class io_uring_poll_sender {
 public:
  /**
   * Completion signatures produced by a poll sender.
   *
   * set_value(ec, unsigned) is the universal observable exit (success,
   * cancel, recoverable failure); set_stopped is reserved for
   * io_context::stop().
   */
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, unsigned), bexec::set_stopped_t()>;

  /**
   * Creates a poll sender for a context, descriptor, and event mask.
   */
  io_uring_poll_sender(io_uring_context& context, descriptor_view descriptor,
                       unsigned poll_mask) noexcept
      : context_(&context), descriptor_(descriptor), poll_mask_(poll_mask) {}

  /**
   * Connects the poll sender to a receiver.
   */
  template <class Receiver>
  auto connect(Receiver receiver) const {
    return io_uring_poll_sender_operation<std::remove_cvref_t<Receiver>>(
        *context_, descriptor_, poll_mask_, std::move(receiver));
  }

 private:
  io_uring_context* context_;
  descriptor_view descriptor_;
  unsigned poll_mask_;
};

/** @cond BNIO_DETAIL */

inline auto io_uring_context::async_poll(
    bnio::async_io::descriptor_view descriptor, unsigned poll_mask) {
  return io_uring_poll_sender(*this, descriptor, poll_mask);
}

/** @endcond */

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_POLL_H_
