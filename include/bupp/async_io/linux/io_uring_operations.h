#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_OPERATIONS_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_OPERATIONS_H_

#include <bupp/async_io/buffer_view.h>
#include <bupp/async_io/descriptor_view.h>
#include <bupp/async_io/ip/endpoint.h>
#include <bupp/async_io/linux/detail/io_uring_receiver_operation.h>
#include <bupp/async_io/linux/socket_address.h>
#include <bupp/async_io/socket_view.h>
#include <bupp/async_io/time.h>
#include <bupp/base/linux/submission_queue_entry.h>
#include <liburing.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bupp::async_io::linux_native {

namespace detail {

/**
 * Converts a chrono duration to Linux's io_uring timeout format.
 */
template <class Rep, class Period>
[[nodiscard]] constexpr __kernel_timespec to_kernel_timespec(
    std::chrono::duration<Rep, Period> value) noexcept {
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(value);
  auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(value - seconds);

  if (nanoseconds.count() < 0) {
    --seconds;
    nanoseconds += std::chrono::seconds(1);
  }

  __kernel_timespec result{};
  result.tv_sec = static_cast<decltype(result.tv_sec)>(seconds.count());
  result.tv_nsec = static_cast<decltype(result.tv_nsec)>(nanoseconds.count());
  return result;
}

/**
 * Converts a chrono time point's epoch duration to Linux's timeout format.
 */
template <class Clock, class Duration>
[[nodiscard]] constexpr __kernel_timespec to_kernel_timespec(
    std::chrono::time_point<Clock, Duration> value) noexcept {
  return to_kernel_timespec(value.time_since_epoch());
}

/**
 * Timeout storage and SQE preparation helper.
 */
class timeout_request {
 public:
  /**
   * Creates a zero-duration timeout request.
   */
  timeout_request() noexcept = default;

  /**
   * Creates a timeout request from a chrono duration.
   */
  explicit timeout_request(bupp::async_io::duration timeout) noexcept {
    reset(timeout);
  }

  /**
   * Replaces the stored relative timeout.
   */
  void reset(bupp::async_io::duration timeout) noexcept {
    timeout_ = to_kernel_timespec(timeout);
  }

  /**
   * Prepares a timeout SQE using the stored timeout.
   */
  void prepare_timeout(bupp::base::submission_queue_entry& sqe, unsigned count,
                       unsigned flags) noexcept {
    sqe.prep_timeout(&timeout_, count, flags);
  }

  /**
   * Prepares a timeout update SQE using the stored timeout.
   */
  void prepare_timeout_update(bupp::base::submission_queue_entry& sqe,
                              std::uint64_t user_data,
                              unsigned flags) noexcept {
    sqe.prep_timeout_update(&timeout_, user_data, flags);
  }

 private:
  __kernel_timespec timeout_{};
};

}  // namespace detail

/**
 * Non-owning C++ view over a mutable native socket message.
 *
 * Copying or moving this view copies only the message pointer. The msghdr
 * object remains owned by the caller.
 */
class mutable_message_view {
 public:
  /**
   * Creates an invalid message view.
   */
  constexpr mutable_message_view() noexcept = default;

  /**
   * Wraps a mutable socket message without taking ownership.
   */
  constexpr explicit mutable_message_view(msghdr& message) noexcept
      : message_(&message) {}

  /**
   * Copies a message view without taking ownership.
   */
  constexpr mutable_message_view(const mutable_message_view&) noexcept =
      default;

  /**
   * Copies a message view without taking ownership.
   */
  constexpr mutable_message_view& operator=(
      const mutable_message_view&) noexcept = default;

  /**
   * Moves a message view by copying the message pointer.
   */
  constexpr mutable_message_view(mutable_message_view&&) noexcept = default;

  /**
   * Moves a message view by copying the message pointer.
   */
  constexpr mutable_message_view& operator=(mutable_message_view&&) noexcept =
      default;

  /**
   * Destroys the view without releasing the message.
   */
  ~mutable_message_view() noexcept = default;

  /**
   * Returns the wrapped native message pointer.
   */
  [[nodiscard]] constexpr msghdr* native_handle() const noexcept {
    return message_;
  }

  /**
   * Returns whether this view references a message.
   */
  [[nodiscard]] constexpr bool valid() const noexcept {
    return message_ != nullptr;
  }

 private:
  msghdr* message_ = nullptr;
};

/**
 * Non-owning C++ view over an immutable native socket message.
 *
 * Copying or moving this view copies only the message pointer. The msghdr
 * object remains owned by the caller.
 */
class const_message_view {
 public:
  /**
   * Creates an invalid message view.
   */
  constexpr const_message_view() noexcept = default;

  /**
   * Wraps an immutable socket message without taking ownership.
   */
  constexpr explicit const_message_view(const msghdr& message) noexcept
      : message_(&message) {}

  /**
   * Copies a message view without taking ownership.
   */
  constexpr const_message_view(const const_message_view&) noexcept = default;

  /**
   * Copies a message view without taking ownership.
   */
  constexpr const_message_view& operator=(const const_message_view&) noexcept =
      default;

  /**
   * Moves a message view by copying the message pointer.
   */
  constexpr const_message_view(const_message_view&&) noexcept = default;

  /**
   * Moves a message view by copying the message pointer.
   */
  constexpr const_message_view& operator=(const_message_view&&) noexcept =
      default;

  /**
   * Destroys the view without releasing the message.
   */
  ~const_message_view() noexcept = default;

  /**
   * Returns the wrapped native message pointer.
   */
  [[nodiscard]] constexpr const msghdr* native_handle() const noexcept {
    return message_;
  }

  /**
   * Returns whether this view references a message.
   */
  [[nodiscard]] constexpr bool valid() const noexcept {
    return message_ != nullptr;
  }

 private:
  const msghdr* message_ = nullptr;
};

/**
 * Non-owning C++ view over a native scatter/gather buffer sequence.
 *
 * Copying or moving this view copies only the iovec pointer and element count.
 * The iovec array remains owned by the caller.
 */
class buffer_sequence_view {
 public:
  /**
   * Creates an empty buffer sequence view.
   */
  constexpr buffer_sequence_view() noexcept = default;

  /**
   * Wraps a native scatter/gather buffer sequence without taking ownership.
   */
  constexpr buffer_sequence_view(const iovec* buffers,
                                 unsigned buffer_count) noexcept
      : buffers_(buffers), buffer_count_(buffer_count) {}

  /**
   * Copies a buffer sequence view without taking ownership.
   */
  constexpr buffer_sequence_view(const buffer_sequence_view&) noexcept =
      default;

  /**
   * Copies a buffer sequence view without taking ownership.
   */
  constexpr buffer_sequence_view& operator=(
      const buffer_sequence_view&) noexcept = default;

  /**
   * Moves a buffer sequence view by copying the pointer and count.
   */
  constexpr buffer_sequence_view(buffer_sequence_view&&) noexcept = default;

  /**
   * Moves a buffer sequence view by copying the pointer and count.
   */
  constexpr buffer_sequence_view& operator=(buffer_sequence_view&&) noexcept =
      default;

  /**
   * Destroys the view without releasing the iovec array.
   */
  ~buffer_sequence_view() noexcept = default;

  /**
   * Returns the native scatter/gather buffer sequence.
   */
  [[nodiscard]] constexpr const iovec* native_data() const noexcept {
    return buffers_;
  }

  /**
   * Returns the number of native scatter/gather buffers.
   */
  [[nodiscard]] constexpr unsigned size() const noexcept {
    return buffer_count_;
  }

  /**
   * Returns whether this view references at least one buffer.
   */
  [[nodiscard]] constexpr bool valid() const noexcept {
    return buffers_ != nullptr || buffer_count_ == 0;
  }

 private:
  const iovec* buffers_ = nullptr;
  unsigned buffer_count_ = 0;
};

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
 * Operation that submits an io_uring no-op request.
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
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_nop();
  }

  /**
   * Starts the no-op operation.
   */
  void start() noexcept { this->start_io(*this); }
};

/**
 * Operation that submits an io_uring timeout request.
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
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
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
  void prepare(bupp::base::submission_queue_entry& sqe) const noexcept {
    sqe.prep_poll_add(descriptor_.native_handle(), poll_mask_);
  }

 private:
  descriptor_view descriptor_;
  unsigned poll_mask_;
};

/**
 * Operation that submits an io_uring poll request.
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
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
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
class io_uring_poll_sender_operation : public io_uring_operation_base {
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
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    request_.prepare(sqe);
  }

  /**
   * Starts the poll or posts an immediate stopped/error completion.
   */
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

  /**
   * Delivers the typed poll completion.
   */
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
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(unsigned),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  /**
   * Creates a poll sender for a context, descriptor, and event mask.
   */
  io_uring_poll_sender(io_uring_context& context, descriptor_view descriptor,
                       unsigned poll_mask) noexcept
      : context_(&context), descriptor_(descriptor), poll_mask_(poll_mask) {}

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

/**
 * Operation that submits an io_uring accept request.
 */
template <class Receiver>
class io_uring_accept_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates an accept operation for a listening socket.
   */
  io_uring_accept_operation(io_uring_context& context,
                            listening_socket_view socket,
                            ip::endpoint& remote_endpoint, int accept_flags,
                            Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        socket_(socket),
        remote_endpoint_(&remote_endpoint),
        accept_flags_(accept_flags) {}

  /**
   * Stores an accepted socket without collecting its remote endpoint.
   */
  io_uring_accept_operation(io_uring_context& context,
                            listening_socket_view socket, int accept_flags,
                            Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        socket_(socket),
        accept_flags_(accept_flags) {}

  /**
   * Prepares the accept SQE.
   *
   * @see io_uring_prep_accept
   */
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    if (remote_endpoint_ == nullptr) {
      sqe.prep_accept(socket_.native_handle(), nullptr, nullptr, accept_flags_);
      return;
    }

    remote_address_ = {};
    remote_address_size_ = static_cast<socklen_t>(sizeof(remote_address_));
    sqe.prep_accept(socket_.native_handle(),
                    reinterpret_cast<sockaddr*>(&remote_address_),
                    &remote_address_size_, accept_flags_);
  }

  /**
   * Converts a completed native peer address back into an async_io endpoint.
   */
  void execute() noexcept override {
    if (remote_endpoint_ != nullptr && this->result >= 0) {
      const auto endpoint =
          make_endpoint(reinterpret_cast<const sockaddr*>(&remote_address_),
                        remote_address_size_);
      if (endpoint.has_value()) {
        *remote_endpoint_ = *endpoint;
      } else {
        remote_endpoint_->reset();
      }
    }
    detail::io_uring_receiver_operation<Receiver>::execute();
  }

  /**
   * Starts the accept operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  listening_socket_view socket_;
  ip::endpoint* remote_endpoint_ = nullptr;
  sockaddr_storage remote_address_{};
  socklen_t remote_address_size_ = 0;
  int accept_flags_;
};

/**
 * Operation that submits an io_uring connect request.
 */
template <class Receiver>
class io_uring_connect_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a connect operation for a socket and native address.
   */
  io_uring_connect_operation(io_uring_context& context,
                             stream_socket_view socket,
                             const ip::endpoint& remote_endpoint,
                             Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        socket_(socket),
        address_(remote_endpoint) {}

  /**
   * Prepares the connect SQE.
   *
   * @see io_uring_prep_connect
   */
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_connect(socket_.native_handle(), address_.data(), address_.size());
  }

  /**
   * Starts the connect operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  stream_socket_view socket_;
  socket_address address_;
};

/**
 * Operation that submits an io_uring recv request.
 */
template <class Receiver>
class io_uring_recv_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a recv operation for a socket buffer.
   */
  io_uring_recv_operation(io_uring_context& context, stream_socket_view socket,
                          const buffer_view& buffer, int recv_flags,
                          Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        socket_(socket),
        buffer_(buffer),
        recv_flags_(recv_flags) {}

  /**
   * Prepares the recv SQE.
   *
   * @see io_uring_prep_recv
   */
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_recv(socket_.native_handle(), buffer_.data, buffer_.size,
                  recv_flags_);
  }

  /**
   * Starts the recv operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  stream_socket_view socket_;
  buffer_view buffer_;
  int recv_flags_;
};

/**
 * Operation that submits an io_uring send request.
 */
template <class Receiver>
class io_uring_send_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a send operation for a socket buffer.
   */
  io_uring_send_operation(io_uring_context& context, stream_socket_view socket,
                          const buffer_view& buffer, int send_flags,
                          Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        socket_(socket),
        buffer_(buffer),
        send_flags_(send_flags) {}

  /**
   * Prepares the send SQE.
   *
   * @see io_uring_prep_send
   */
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_send(socket_.native_handle(), buffer_.data, buffer_.size,
                  send_flags_);
  }

  /**
   * Starts the send operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  stream_socket_view socket_;
  buffer_view buffer_;
  int send_flags_;
};

/**
 * Operation that submits an io_uring recvmsg request.
 */
template <class Receiver>
class io_uring_recvmsg_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a recvmsg operation for a socket message.
   */
  io_uring_recvmsg_operation(io_uring_context& context,
                             stream_socket_view socket,
                             mutable_message_view message,
                             unsigned message_flags, Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        socket_(socket),
        message_(message),
        message_flags_(message_flags) {}

  /**
   * Prepares the recvmsg SQE.
   *
   * @see io_uring_prep_recvmsg
   */
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_recvmsg(socket_.native_handle(), message_.native_handle(),
                     message_flags_);
  }

  /**
   * Starts the recvmsg operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  stream_socket_view socket_;
  mutable_message_view message_;
  unsigned message_flags_;
};

/**
 * Operation that submits an io_uring sendmsg request.
 */
template <class Receiver>
class io_uring_sendmsg_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a sendmsg operation for a socket message.
   */
  io_uring_sendmsg_operation(io_uring_context& context,
                             stream_socket_view socket,
                             const_message_view message, unsigned message_flags,
                             Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        socket_(socket),
        message_(message),
        message_flags_(message_flags) {}

  /**
   * Prepares the sendmsg SQE.
   *
   * @see io_uring_prep_sendmsg
   */
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_sendmsg(socket_.native_handle(), message_.native_handle(),
                     message_flags_);
  }

  /**
   * Starts the sendmsg operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  stream_socket_view socket_;
  const_message_view message_;
  unsigned message_flags_;
};

/**
 * Operation that submits an io_uring read request.
 */
template <class Receiver>
class io_uring_read_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a read operation for a file descriptor buffer.
   */
  io_uring_read_operation(io_uring_context& context, descriptor_view descriptor,
                          const buffer_view& buffer, std::uint64_t offset,
                          Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        descriptor_(descriptor),
        buffer_(buffer),
        offset_(offset) {}

  /**
   * Prepares the read SQE.
   *
   * @see io_uring_prep_read
   */
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_read(descriptor_.native_handle(), buffer_.data,
                  static_cast<unsigned>(buffer_.size), offset_);
  }

  /**
   * Starts the read operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  descriptor_view descriptor_;
  buffer_view buffer_;
  std::uint64_t offset_;
};

/**
 * Operation that submits an io_uring write request.
 */
template <class Receiver>
class io_uring_write_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a write operation for a file descriptor buffer.
   */
  io_uring_write_operation(io_uring_context& context,
                           descriptor_view descriptor,
                           const buffer_view& buffer, std::uint64_t offset,
                           Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        descriptor_(descriptor),
        buffer_(buffer),
        offset_(offset) {}

  /**
   * Prepares the write SQE.
   *
   * @see io_uring_prep_write
   */
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_write(descriptor_.native_handle(), buffer_.data,
                   static_cast<unsigned>(buffer_.size), offset_);
  }

  /**
   * Starts the write operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  descriptor_view descriptor_;
  buffer_view buffer_;
  std::uint64_t offset_;
};

/**
 * Operation that submits an io_uring readv request.
 */
template <class Receiver>
class io_uring_readv_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a readv operation for a file descriptor and iovec array.
   */
  io_uring_readv_operation(io_uring_context& context,
                           descriptor_view descriptor,
                           buffer_sequence_view buffers, std::uint64_t offset,
                           Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        descriptor_(descriptor),
        buffers_(buffers),
        offset_(offset) {}

  /**
   * Prepares the readv SQE.
   *
   * @see io_uring_prep_readv
   */
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_readv(descriptor_.native_handle(), buffers_.native_data(),
                   buffers_.size(), offset_);
  }

  /**
   * Starts the readv operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  descriptor_view descriptor_;
  buffer_sequence_view buffers_;
  std::uint64_t offset_;
};

/**
 * Operation that submits an io_uring writev request.
 */
template <class Receiver>
class io_uring_writev_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a writev operation for a file descriptor and iovec array.
   */
  io_uring_writev_operation(io_uring_context& context,
                            descriptor_view descriptor,
                            buffer_sequence_view buffers, std::uint64_t offset,
                            Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        descriptor_(descriptor),
        buffers_(buffers),
        offset_(offset) {}

  /**
   * Prepares the writev SQE.
   *
   * @see io_uring_prep_writev
   */
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_writev(descriptor_.native_handle(), buffers_.native_data(),
                    buffers_.size(), offset_);
  }

  /**
   * Starts the writev operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  descriptor_view descriptor_;
  buffer_sequence_view buffers_;
  std::uint64_t offset_;
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
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    prepare_(sqe);
  }

  /**
   * Starts the raw operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  std::remove_cvref_t<Prepare> prepare_;
};

/**
 * Operation that posts a synchronous DNS query onto an io_uring_context.
 */
template <class Receiver>
class io_uring_resolve_operation : public io_uring_operation_base {
 public:
  /**
   * Creates a DNS resolution operation for a context and query.
   */
  io_uring_resolve_operation(io_uring_context& context,
                             bupp::async_io::dns_query query,
                             bupp::async_io::dns_result_view result,
                             Receiver receiver)
      : context_(&context),
        query_(std::move(query)),
        result_(result),
        receiver_(std::move(receiver)) {}

  /**
   * Copy construction is disabled because operations are queued intrusively.
   */
  io_uring_resolve_operation(const io_uring_resolve_operation&) = delete;

  /**
   * Copy assignment is disabled because operations are queued intrusively.
   */
  io_uring_resolve_operation& operator=(const io_uring_resolve_operation&) =
      delete;

  /**
   * Move construction is disabled because operations are queued intrusively.
   */
  io_uring_resolve_operation(io_uring_resolve_operation&&) = delete;

  /**
   * Move assignment is disabled because operations are queued intrusively.
   */
  io_uring_resolve_operation& operator=(io_uring_resolve_operation&&) = delete;

  /**
   * Destroys the operation without delivering an additional signal.
   */
  ~io_uring_resolve_operation() noexcept override = default;

  /**
   * Posts this operation to the context run loop.
   */
  void start() noexcept {
    if (stop_requested()) {
      completion_ = completion_kind::stopped;
      (void)context_->post(*this);
      return;
    }

    completion_ = completion_kind::value;
    (void)context_->post(*this);
  }

  /**
   * Executes the synchronous resolver and completes the receiver.
   */
  void execute() noexcept override {
    switch (completion_) {
      case completion_kind::value:
        complete_resolve();
        break;
      case completion_kind::stopped:
        bexec::set_stopped(std::move(receiver_));
        break;
    }
  }

 private:
  enum class completion_kind {
    value,
    stopped,
  };

  /**
   * Returns whether the receiver environment has requested cancellation.
   */
  [[nodiscard]] bool stop_requested() const noexcept {
    auto env = bexec::get_env(receiver_);
    auto token = bexec::query(env, bexec::get_stop_token);
    return token.stop_requested();
  }

  /**
   * Runs the async_io platform resolver helper.
   */
  void complete_resolve() noexcept {
    std::size_t count = 0;
    const std::error_code error =
        bupp::async_io::resolve_dns(query_, result_, count);
    if (error) {
      bexec::set_error(std::move(receiver_), error);
    } else {
      bexec::set_value(std::move(receiver_), count);
    }
  }

  io_uring_context* context_;
  bupp::async_io::dns_query query_;
  bupp::async_io::dns_result_view result_;
  std::remove_cvref_t<Receiver> receiver_;
  completion_kind completion_ = completion_kind::value;
};

/**
 * Sender returned by io_uring_context DNS resolution APIs.
 */
class io_uring_resolve_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  /**
   * Creates a DNS sender for a context and query.
   */
  io_uring_resolve_sender(io_uring_context& context,
                          bupp::async_io::dns_query query,
                          bupp::async_io::dns_result_view result)
      : context_(&context), query_(std::move(query)), result_(result) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return io_uring_resolve_operation<std::remove_cvref_t<Receiver>>(
        *context_, std::move(query_), result_, std::move(receiver));
  }

  template <class Receiver>
  auto connect(Receiver receiver) const& {
    return io_uring_resolve_operation<std::remove_cvref_t<Receiver>>(
        *context_, query_, result_, std::move(receiver));
  }

 private:
  io_uring_context* context_;
  bupp::async_io::dns_query query_;
  bupp::async_io::dns_result_view result_;
};

inline auto io_uring_context::async_poll(
    bupp::async_io::descriptor_view descriptor, unsigned poll_mask) {
  return io_uring_poll_sender(*this, descriptor, poll_mask);
}

inline auto io_uring_context::async_resolve(
    bupp::async_io::dns_query query, bupp::async_io::dns_result_view result) {
  return io_uring_resolve_sender(*this, std::move(query), result);
}

inline auto io_uring_context::async_resolve(
    std::string_view host, std::string_view service,
    bupp::async_io::dns_result_view result) {
  return async_resolve(bupp::async_io::dns_query(host, service), result);
}

}  // namespace bupp::async_io::linux_native

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_OPERATIONS_H_
