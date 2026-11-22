/**
 * @file socket.h
 * @brief kqueue socket operations (accept, connect, read, write).
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_SOCKET_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_SOCKET_H_

#include <bnio/async_io/bsd/kqueue_operations/detail/io_request.h>
#include <bnio/async_io/bsd/kqueue_operations/detail/native_io.h>
#include <bnio/async_io/bsd/socket_address.h>
#include <bnio/async_io/buffer_view.h>
#include <bnio/async_io/ip/endpoint.h>
#include <bnio/async_io/local/endpoint.h>
#include <bnio/async_io/local/socket_view.h>
#include <bnio/async_io/socket_view.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <bexec/completion_signatures.hpp>
#include <cerrno>
#include <cstddef>
#include <system_error>
#include <utility>

namespace bnio::async_io::bsd_native {

namespace detail {

/** Encodes a nonblocking syscall result: bytes transferred, or -errno;
 *  EINTR, EAGAIN, EWOULDBLOCK, EINPROGRESS, and EALREADY map to -EAGAIN. */
[[nodiscard]] inline int nonblocking_io_result(ssize_t result) noexcept {
  if (result >= 0) {
    return static_cast<int>(result);
  }
  const int error = errno;
  if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK ||
      error == EINPROGRESS || error == EALREADY) {
    return -EAGAIN;
  }
  return -error;
}

/** Byte-count completion: set_value(ec, size) or set_stopped(). */
using size_completion_signatures = bexec::completion_signatures<
    bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

}  // namespace detail

/** One nonblocking recv request, completed after EVFILT_READ when necessary. */
class kqueue_receive_request {
 public:
  /** Completion signals: set_value(ec, bytes) or set_stopped(). */
  using completion_signatures = detail::size_completion_signatures;

  /** Binds the descriptor, buffer, and native recv flags. */
  kqueue_receive_request(int descriptor, buffer_view buffer, int flags) noexcept
      : descriptor_(descriptor), buffer_(buffer), flags_(flags) {}

  /** Registers the descriptor for read readiness with @p helper. */
  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_read(descriptor_);
  }

  /** Attempts one immediate nonblocking receive. */
  [[nodiscard]] int start_io() noexcept { return perform_io(); }

  /** Skips the initial recv; the operation waits for read readiness. */
  [[nodiscard]] int start_deferred() noexcept { return -EAGAIN; }

  /** Performs one nonblocking recv and normalizes the syscall result. */
  [[nodiscard]] int perform_io() noexcept {
    if (buffer_.size > 0 && buffer_.data == nullptr) {
      return -EFAULT;
    }
    return detail::nonblocking_io_result(
        ::recv(descriptor_, buffer_.data, detail::bounded_io_size(buffer_.size),
               flags_ | MSG_DONTWAIT));
  }

  /** Delivers the received byte count, clamping negative results to zero. */
  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  int descriptor_;
  buffer_view buffer_;
  int flags_;
};

/** One nonblocking send request, completed after EVFILT_WRITE when necessary.
 */
class kqueue_send_request {
 public:
  /** Completion signals: set_value(ec, bytes) or set_stopped(). */
  using completion_signatures = detail::size_completion_signatures;

  /** Binds the descriptor, data, size, and native send flags. */
  kqueue_send_request(int descriptor, const void* data, std::size_t size,
                      int flags) noexcept
      : descriptor_(descriptor), data_(data), size_(size), flags_(flags) {}

  /** Registers the descriptor for write readiness with @p helper. */
  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_write(descriptor_);
  }

  /** Attempts one immediate nonblocking send. */
  [[nodiscard]] int start_io() noexcept { return perform_io(); }

  /** Skips the initial send; the operation waits for write readiness. */
  [[nodiscard]] int start_deferred() noexcept { return -EAGAIN; }

  /** Performs one nonblocking send and normalizes the syscall result. */
  [[nodiscard]] int perform_io() noexcept {
    if (size_ > 0 && data_ == nullptr) {
      return -EFAULT;
    }
    return detail::nonblocking_io_result(::send(descriptor_, data_,
                                                detail::bounded_io_size(size_),
                                                flags_ | MSG_DONTWAIT));
  }

  /** Delivers the sent byte count, clamping negative results to zero. */
  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  int descriptor_;
  const void* data_;
  std::size_t size_;
  int flags_;
};

/** One nonblocking recvfrom request with endpoint conversion. */
class kqueue_receive_from_request {
 public:
  /** Completion signals: set_value(ec, bytes) or set_stopped(). */
  using completion_signatures = detail::size_completion_signatures;

  /** Binds the socket, buffer, output endpoint, and native flags. */
  kqueue_receive_from_request(datagram_socket_view socket, buffer_view buffer,
                              ip::endpoint& endpoint, int flags) noexcept
      : descriptor_(socket.native_handle()),
        buffer_(buffer),
        endpoint_(&endpoint),
        flags_(flags) {}

  /** Registers the descriptor for read readiness with @p helper. */
  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_read(descriptor_);
  }

  /** Attempts one immediate nonblocking receive-from. */
  [[nodiscard]] int start_io() noexcept { return perform_io(); }

  /** Skips the initial recvfrom; the operation waits for read readiness. */
  [[nodiscard]] int start_deferred() noexcept { return -EAGAIN; }

  /** Performs one nonblocking recvfrom, capturing the peer address. */
  [[nodiscard]] int perform_io() noexcept {
    if (buffer_.size > 0 && buffer_.data == nullptr) {
      return -EFAULT;
    }
    remote_address_ = {};
    socklen_t size = sizeof(remote_address_);
    const ssize_t result =
        ::recvfrom(descriptor_, buffer_.data,
                   detail::bounded_io_size(buffer_.size), flags_ | MSG_DONTWAIT,
                   reinterpret_cast<sockaddr*>(&remote_address_), &size);
    if (result >= 0) {
      remote_size_ = size;
    }
    return detail::nonblocking_io_result(result);
  }

  /** Delivers the byte count; on success converts the captured peer address
   *  into @p endpoint. An undecodable family reports
   *  address_family_not_supported with a reset endpoint. */
  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    if (result >= 0 && !ec) {
      const auto endpoint = make_endpoint(
          reinterpret_cast<const sockaddr*>(&remote_address_), remote_size_);
      if (!endpoint.has_value()) {
        // endpoint decode failure: override ec with
        // address_family_not_supported
        endpoint_->reset();
        bexec::set_value(
            std::forward<Receiver>(receiver),
            std::make_error_code(std::errc::address_family_not_supported),
            std::size_t{0});
        return;
      }
      *endpoint_ = *endpoint;
    }
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  int descriptor_;
  buffer_view buffer_;
  ip::endpoint* endpoint_;
  sockaddr_storage remote_address_{};
  socklen_t remote_size_ = sizeof(remote_address_);
  int flags_;
};

/** Local recvfrom request; same contract as kqueue_receive_from_request
 *  with a local::endpoint output. */
class kqueue_local_receive_from_request {
 public:
  /** Completion signals: set_value(ec, bytes) or set_stopped(). */
  using completion_signatures = detail::size_completion_signatures;

  /** Binds the socket, buffer, output endpoint, and native flags. */
  kqueue_local_receive_from_request(local::datagram_socket_view socket,
                                    buffer_view buffer,
                                    local::endpoint& endpoint,
                                    int flags) noexcept
      : descriptor_(socket.native_handle()),
        buffer_(buffer),
        endpoint_(&endpoint),
        flags_(flags) {}

  /** Registers the descriptor for read readiness with @p helper. */
  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_read(descriptor_);
  }

  /** Attempts one immediate nonblocking receive-from. */
  [[nodiscard]] int start_io() noexcept { return perform_io(); }

  /** Skips the initial recvfrom; the operation waits for read readiness. */
  [[nodiscard]] int start_deferred() noexcept { return -EAGAIN; }

  /** Performs one nonblocking recvfrom, capturing the peer address. */
  [[nodiscard]] int perform_io() noexcept {
    if (buffer_.size > 0 && buffer_.data == nullptr) {
      return -EFAULT;
    }
    remote_address_ = {};
    socklen_t size = sizeof(remote_address_);
    const ssize_t result =
        ::recvfrom(descriptor_, buffer_.data,
                   detail::bounded_io_size(buffer_.size), flags_ | MSG_DONTWAIT,
                   reinterpret_cast<sockaddr*>(&remote_address_), &size);
    if (result >= 0) {
      remote_size_ = size;
    }
    return detail::nonblocking_io_result(result);
  }

  /** Delivers the byte count; on success converts the captured peer address
   *  into @p endpoint. An undecodable family reports
   *  address_family_not_supported with a reset endpoint. */
  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    if (result >= 0 && !ec) {
      const auto endpoint = make_local_endpoint(
          reinterpret_cast<const sockaddr*>(&remote_address_), remote_size_);
      if (!endpoint.has_value()) {
        // endpoint decode failure: override ec with
        // address_family_not_supported
        *endpoint_ = local::endpoint();
        bexec::set_value(
            std::forward<Receiver>(receiver),
            std::make_error_code(std::errc::address_family_not_supported),
            std::size_t{0});
        return;
      }
      *endpoint_ = *endpoint;
    }
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  int descriptor_;
  buffer_view buffer_;
  local::endpoint* endpoint_;
  sockaddr_storage remote_address_{};
  socklen_t remote_size_ = sizeof(remote_address_);
  int flags_;
};

/** One nonblocking sendto request with owned native destination storage. */
class kqueue_send_to_request {
 public:
  /** Completion signals: set_value(ec, bytes) or set_stopped(). */
  using completion_signatures = detail::size_completion_signatures;

  /** Binds the socket, data, size, destination endpoint, and flags. */
  kqueue_send_to_request(datagram_socket_view socket, const void* data,
                         std::size_t size, const ip::endpoint& endpoint,
                         int flags) noexcept
      : descriptor_(socket.native_handle()),
        data_(data),
        size_(size),
        remote_address_(endpoint),
        flags_(flags) {}

  /** Binds the local socket, data, size, destination endpoint, and flags. */
  kqueue_send_to_request(local::datagram_socket_view socket, const void* data,
                         std::size_t size, const local::endpoint& endpoint,
                         int flags) noexcept
      : descriptor_(socket.native_handle()),
        data_(data),
        size_(size),
        remote_address_(endpoint),
        flags_(flags) {}

  /** Registers the descriptor for write readiness with @p helper. */
  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_write(descriptor_);
  }

  /** Attempts one immediate nonblocking send-to. */
  [[nodiscard]] int start_io() noexcept { return perform_io(); }

  /** Skips the initial sendto; the operation waits for write readiness. */
  [[nodiscard]] int start_deferred() noexcept { return -EAGAIN; }

  /** Performs one nonblocking sendto to the stored destination. */
  [[nodiscard]] int perform_io() noexcept {
    if (size_ > 0 && data_ == nullptr) {
      return -EFAULT;
    }
    return detail::nonblocking_io_result(::sendto(
        descriptor_, data_, detail::bounded_io_size(size_),
        flags_ | MSG_DONTWAIT, remote_address_.data(), remote_address_.size()));
  }

  /** Delivers the sent byte count, clamping negative results to zero. */
  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  int descriptor_;
  const void* data_;
  std::size_t size_;
  socket_address remote_address_;
  int flags_;
};

/** One nonblocking accept request. */
class kqueue_accept_request {
 public:
  /** Completion signals: set_value(ec, accepted descriptor) or
   *  set_stopped(). */
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code, int),
                                   bexec::set_stopped_t()>;

  /** Binds the listening socket and native accept flags. */
  kqueue_accept_request(stream_socket_view socket, int flags) noexcept
      : descriptor_(socket.native_handle()), flags_(flags) {}

  /** Binds a listening local stream socket and native accept flags. */
  kqueue_accept_request(local::stream_socket_view socket, int flags) noexcept
      : descriptor_(socket.native_handle()), flags_(flags) {}

  /** Registers the listening socket for read readiness with @p helper. */
  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_read(descriptor_);
  }

  /** Attempts one immediate nonblocking accept. */
  [[nodiscard]] int start_io() noexcept { return perform_io(); }

  /** Skips the initial accept; the operation waits for listener read
   *  readiness. */
  [[nodiscard]] int start_deferred() noexcept { return -EAGAIN; }

  /** Accepts one connection, applying nonblocking and close-on-exec flags
   *  to the accepted descriptor. */
  [[nodiscard]] int perform_io() noexcept {
    int supported_flags = 0;
#if defined(SOCK_CLOEXEC)
    supported_flags |= SOCK_CLOEXEC;
#endif
#if defined(SOCK_NONBLOCK)
    supported_flags |= SOCK_NONBLOCK;
#endif
    if ((flags_ & ~supported_flags) != 0) {
      return -EINVAL;
    }
    const int accepted = ::accept(descriptor_, nullptr, nullptr);
    if (accepted < 0) {
      return detail::nonblocking_io_result(-1);
    }

    const int nonblocking = detail::set_descriptor_nonblocking(accepted);
    if (nonblocking < 0) {
      (void)::close(accepted);
      return nonblocking;
    }
#if defined(SOCK_CLOEXEC)
    if ((flags_ & SOCK_CLOEXEC) != 0 &&
        ::fcntl(accepted, F_SETFD, FD_CLOEXEC) != 0) {
      const int error = errno;
      (void)::close(accepted);
      return -error;
    }
#endif
    return accepted;
  }

  /** Forwards the accepted descriptor or error to @p receiver unchanged. */
  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec, result);
  }

 private:
  int descriptor_;
  int flags_;
};

/** One nonblocking connect request followed by SO_ERROR after readiness.
 *  Not deferrable: EVFILT_WRITE only reports a connection-oriented socket as
 *  writable once ::connect() is in progress, so perform_io() must issue the
 *  syscall before the operation registers. */
class kqueue_connect_request {
 public:
  /** Completion signals: set_value(ec) or set_stopped(). */
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                   bexec::set_stopped_t()>;

  /** Constructs the request from the socket and the remote endpoint. */
  kqueue_connect_request(stream_socket_view socket,
                         const ip::endpoint& endpoint) noexcept
      : descriptor_(socket.native_handle()), address_(endpoint) {}

  /** Binds a local stream socket and local remote endpoint. */
  kqueue_connect_request(local::stream_socket_view socket,
                         const local::endpoint& endpoint) noexcept
      : descriptor_(socket.native_handle()), address_(endpoint) {}

  /** Registers the socket for write readiness with @p helper. */
  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_write(descriptor_);
  }

  /** Makes the socket nonblocking and starts the connect attempt. */
  [[nodiscard]] int start_io() noexcept {
    const int nonblocking = detail::set_descriptor_nonblocking(descriptor_);
    return nonblocking < 0 ? nonblocking : perform_io();
  }

  /** Starts the nonblocking connect, or reads SO_ERROR once the socket is
   *  writable; returns -EAGAIN while the connection is still in progress. */
  [[nodiscard]] int perform_io() noexcept {
    if (!initiated_) {
      const int nonblocking = detail::set_descriptor_nonblocking(descriptor_);
      if (nonblocking < 0) {
        return nonblocking;
      }
      initiated_ = true;
      const int rc = ::connect(descriptor_, address_.data(), address_.size());
      if (rc == 0 || errno == EISCONN) {
        return 0;
      }
      if (errno == EINPROGRESS || errno == EALREADY) {
        return -EAGAIN;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return -EAGAIN;
      }
      return -errno;
    }
    int error = 0;
    socklen_t size = sizeof(error);
    if (::getsockopt(descriptor_, SOL_SOCKET, SO_ERROR, &error, &size) != 0) {
      return -errno;
    }
    if (error == EINPROGRESS || error == EALREADY || error == EWOULDBLOCK) {
      return -EAGAIN;
    }
    return error == 0 ? 0 : -error;
  }

  /** Delivers the connect outcome to @p receiver. */
  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec);
  }

 private:
  int descriptor_;
  socket_address address_;
  bool initiated_ = false;
};

/** Sender returned by kqueue_context::async_receive. */
using kqueue_receive_sender =
    detail::kqueue_ready_io_sender<kqueue_receive_request>;

/** Sender returned by kqueue_context::async_send. */
using kqueue_send_sender = detail::kqueue_ready_io_sender<kqueue_send_request>;

/** Sender returned by kqueue_context::async_receive_from. */
using kqueue_receive_from_sender =
    detail::kqueue_ready_io_sender<kqueue_receive_from_request>;

/** Sender returned by kqueue_context::async_send_to. */
using kqueue_send_to_sender =
    detail::kqueue_ready_io_sender<kqueue_send_to_request>;

/** Sender returned by kqueue_context::async_accept. */
using kqueue_accept_sender =
    detail::kqueue_ready_io_sender<kqueue_accept_request>;

/** Sender returned by kqueue_context::async_connect. */
using kqueue_connect_sender =
    detail::kqueue_ready_io_sender<kqueue_connect_request>;

/** @cond BNIO_DETAIL */

inline auto kqueue_context::async_receive(stream_socket_view socket,
                                          buffer_view buffer, int flags) {
  return kqueue_receive_sender(
      *this, kqueue_receive_request(socket.native_handle(), buffer, flags));
}

inline auto kqueue_context::async_send(stream_socket_view socket,
                                       const void* data, std::size_t size,
                                       int flags) {
  return kqueue_send_sender(
      *this, kqueue_send_request(socket.native_handle(), data, size, flags));
}

inline auto kqueue_context::async_receive(datagram_socket_view socket,
                                          buffer_view buffer, int flags) {
  return kqueue_receive_sender(
      *this, kqueue_receive_request(socket.native_handle(), buffer, flags));
}

inline auto kqueue_context::async_send(datagram_socket_view socket,
                                       const void* data, std::size_t size,
                                       int flags) {
  return kqueue_send_sender(
      *this, kqueue_send_request(socket.native_handle(), data, size, flags));
}

inline auto kqueue_context::async_receive_from(datagram_socket_view socket,
                                               buffer_view buffer,
                                               ip::endpoint& endpoint,
                                               int flags) {
  return kqueue_receive_from_sender(
      *this, kqueue_receive_from_request(socket, buffer, endpoint, flags));
}

inline auto kqueue_context::async_send_to(datagram_socket_view socket,
                                          const void* data, std::size_t size,
                                          const ip::endpoint& endpoint,
                                          int flags) {
  return kqueue_send_to_sender(
      *this, kqueue_send_to_request(socket, data, size, endpoint, flags));
}

inline auto kqueue_context::async_accept(stream_socket_view socket, int flags) {
  return kqueue_accept_sender(*this, kqueue_accept_request(socket, flags));
}

inline auto kqueue_context::async_connect(stream_socket_view socket,
                                          const ip::endpoint& endpoint) {
  return kqueue_connect_sender(*this, kqueue_connect_request(socket, endpoint));
}

/** @endcond */

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_SOCKET_H_
