#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_OPERATIONS_SOCKET_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_OPERATIONS_SOCKET_H_

#include <bupp/async_io/buffer_view.h>
#include <bupp/async_io/ip/endpoint.h>
#include <bupp/async_io/linux/detail/io_uring_receiver_operation.h>
#include <bupp/async_io/linux/io_uring_operations/helpers.h>
#include <bupp/async_io/linux/io_uring_operations/views.h>
#include <bupp/async_io/linux/socket_address.h>
#include <bupp/async_io/socket_view.h>
#include <bupp/base/linux/submission_queue_entry.h>
#include <sys/socket.h>

#include <utility>

namespace bupp::async_io::linux_native {

/**
 * Operation representing an io_uring accept request.
 */
template <class Receiver>
class io_uring_accept_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates an accept operation for a listening socket.
   */
  io_uring_accept_operation(io_uring_context& context,
                            stream_socket_view socket,
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
                            stream_socket_view socket, int accept_flags,
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
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
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
  stream_socket_view socket_;
  ip::endpoint* remote_endpoint_ = nullptr;
  sockaddr_storage remote_address_{};
  socklen_t remote_address_size_ = 0;
  int accept_flags_;
};

/**
 * Operation representing an io_uring connect request.
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
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
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
 * Operation representing an io_uring recv request.
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
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_recv(socket_.native_handle(), buffer_.data,
                  detail::bounded_io_size(buffer_.size), recv_flags_);
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
 * Operation representing an io_uring send request.
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
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_send(socket_.native_handle(), buffer_.data,
                  detail::bounded_io_size(buffer_.size), send_flags_);
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
 * Operation that receives one datagram from a connected default peer.
 */
template <class Receiver>
class io_uring_datagram_receive_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  io_uring_datagram_receive_operation(io_uring_context& context,
                                      datagram_socket_view socket,
                                      const buffer_view& buffer,
                                      int receive_flags, Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        socket_(socket),
        buffer_(buffer),
        receive_flags_(receive_flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_recv(socket_.native_handle(), buffer_.data,
                  detail::bounded_io_size(buffer_.size), receive_flags_);
  }

  void start() noexcept { this->start_io(*this); }

 private:
  datagram_socket_view socket_;
  buffer_view buffer_;
  int receive_flags_;
};

/**
 * Operation that sends one datagram to a connected default peer.
 */
template <class Receiver>
class io_uring_datagram_send_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  io_uring_datagram_send_operation(io_uring_context& context,
                                   datagram_socket_view socket,
                                   const buffer_view& buffer, int send_flags,
                                   Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        socket_(socket),
        buffer_(buffer),
        send_flags_(send_flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_send(socket_.native_handle(), buffer_.data,
                  detail::bounded_io_size(buffer_.size), send_flags_);
  }

  void start() noexcept { this->start_io(*this); }

 private:
  datagram_socket_view socket_;
  buffer_view buffer_;
  int send_flags_;
};

/**
 * Operation representing an io_uring recvmsg request.
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
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
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
 * Operation representing an io_uring sendmsg request.
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
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
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
 * Operation that receives one datagram and captures its source endpoint.
 */
template <class Receiver>
class io_uring_receive_from_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  io_uring_receive_from_operation(io_uring_context& context,
                                  datagram_socket_view socket,
                                  const buffer_view& buffer,
                                  ip::endpoint& remote_endpoint,
                                  unsigned receive_flags, Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        socket_(socket),
        buffer_(buffer),
        remote_endpoint_(&remote_endpoint),
        receive_flags_(receive_flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    remote_address_ = {};
    buffer_entry_ = {buffer_.data, detail::bounded_io_size(buffer_.size)};
    message_ = {};
    message_.msg_name = &remote_address_;
    message_.msg_namelen = sizeof(remote_address_);
    message_.msg_iov = &buffer_entry_;
    message_.msg_iovlen = 1;
    sqe.prep_recvmsg(socket_.native_handle(), &message_, receive_flags_);
  }

  void execute() noexcept override {
    if (this->result >= 0) {
      const auto endpoint =
          make_endpoint(reinterpret_cast<const sockaddr*>(&remote_address_),
                        message_.msg_namelen);
      if (endpoint.has_value()) {
        *remote_endpoint_ = *endpoint;
      } else {
        remote_endpoint_->reset();
      }
    }
    detail::io_uring_receiver_operation<Receiver>::execute();
  }

  void start() noexcept { this->start_io(*this); }

 private:
  datagram_socket_view socket_;
  buffer_view buffer_;
  ip::endpoint* remote_endpoint_;
  sockaddr_storage remote_address_{};
  iovec buffer_entry_{};
  msghdr message_{};
  unsigned receive_flags_;
};

/**
 * Operation that sends one datagram to an explicit destination endpoint.
 */
template <class Receiver>
class io_uring_send_to_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  io_uring_send_to_operation(io_uring_context& context,
                             datagram_socket_view socket,
                             const buffer_view& buffer,
                             const ip::endpoint& remote_endpoint,
                             unsigned send_flags, Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        socket_(socket),
        buffer_(buffer),
        remote_address_(remote_endpoint),
        send_flags_(send_flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    buffer_entry_ = {buffer_.data, detail::bounded_io_size(buffer_.size)};
    message_ = {};
    message_.msg_name = const_cast<sockaddr*>(remote_address_.data());
    message_.msg_namelen = remote_address_.size();
    message_.msg_iov = &buffer_entry_;
    message_.msg_iovlen = 1;
    sqe.prep_sendmsg(socket_.native_handle(), &message_, send_flags_);
  }

  void start() noexcept { this->start_io(*this); }

 private:
  datagram_socket_view socket_;
  buffer_view buffer_;
  socket_address remote_address_;
  iovec buffer_entry_{};
  msghdr message_{};
  unsigned send_flags_;
};

}  // namespace bupp::async_io::linux_native

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_OPERATIONS_SOCKET_H_
