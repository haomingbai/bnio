/**
 * @file socket_view.h
 * @brief Non-owning local (AF_UNIX) socket views.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LOCAL_SOCKET_VIEW_H_
#define BNIO_ASYNC_IO_LOCAL_SOCKET_VIEW_H_

#include <bnio/async_io/local/endpoint.h>
#include <bnio/export.h>

#include <system_error>

namespace bnio::async_io::local {

/**
 * Non-owning view over a native local stream socket descriptor.
 *
 * Listening is a lifecycle state of a stream socket, not a separate socket
 * kind, so the same view covers unbound, bound, listening, and connected
 * sockets. Synchronous operations cover bind(), listen(), connect(),
 * shutdown(), and endpoint queries; stream transfer is async-only. As with
 * every socket view, destroying the view does nothing: the socket remains
 * owned and closed by the caller.
 */
class BNIO_EXPORT stream_socket_view {
 public:
  /**
   * Native socket descriptor type.
   */
  using native_handle_type = int;

  /**
   * Creates an invalid local stream socket view.
   */
  constexpr stream_socket_view() noexcept = default;

  /**
   * Wraps a native socket descriptor without taking ownership.
   */
  constexpr explicit stream_socket_view(native_handle_type fd) noexcept
      : fd_(fd) {}

  /**
   * Returns the wrapped native socket descriptor.
   */
  [[nodiscard]] constexpr native_handle_type native_handle() const noexcept {
    return fd_;
  }

  /**
   * Returns whether this view references a valid descriptor value.
   */
  [[nodiscard]] constexpr bool valid() const noexcept { return fd_ >= 0; }

  /**
   * Binds the socket to a local endpoint. An endpoint that cannot be
   * converted to sockaddr_un reports std::errc::invalid_argument without
   * a syscall.
   */
  [[nodiscard]] std::error_code bind(const endpoint& endpoint) noexcept;

  /**
   * Marks the bound stream socket as listening.
   */
  [[nodiscard]] std::error_code listen(int backlog) noexcept;

  /**
   * Connects the socket to a local endpoint. An endpoint that cannot be
   * converted to sockaddr_un reports std::errc::invalid_argument without
   * a syscall.
   */
  [[nodiscard]] std::error_code connect(const endpoint& endpoint) noexcept;

  /**
   * Shuts down socket send and/or receive operations.
   */
  [[nodiscard]] std::error_code shutdown(int how) noexcept;

  /**
   * Stores the connected peer endpoint into @p endpoint, resetting it to
   * the unspecified kind for an unnamed peer.
   */
  [[nodiscard]] std::error_code remote_endpoint(
      endpoint& endpoint) const noexcept;

 private:
  native_handle_type fd_ = -1;
};

/**
 * Non-owning view over a connectionless or connected local datagram
 * socket.
 *
 * Datagram operations stay distinct from stream operations so stream
 * partial I/O rules can never be applied to a datagram. As with every
 * socket view, destroying the view does nothing: the socket remains owned
 * and closed by the caller.
 */
class BNIO_EXPORT datagram_socket_view {
 public:
  /**
   * Native socket descriptor type.
   */
  using native_handle_type = stream_socket_view::native_handle_type;

  /**
   * Creates an invalid local datagram socket view.
   */
  constexpr datagram_socket_view() noexcept = default;

  /**
   * Wraps a native socket descriptor without taking ownership.
   */
  constexpr explicit datagram_socket_view(native_handle_type fd) noexcept
      : fd_(fd) {}

  /**
   * Returns the wrapped native socket descriptor.
   */
  [[nodiscard]] constexpr native_handle_type native_handle() const noexcept {
    return fd_;
  }

  /**
   * Returns whether this view references a valid descriptor value.
   */
  [[nodiscard]] constexpr bool valid() const noexcept { return fd_ >= 0; }

  /**
   * Binds the socket to a local endpoint. An endpoint that cannot be
   * converted to sockaddr_un reports std::errc::invalid_argument without
   * a syscall.
   */
  [[nodiscard]] std::error_code bind(const endpoint& endpoint) noexcept;

  /**
   * Sets the default peer used by subsequent send and receive calls.
   */
  [[nodiscard]] std::error_code connect(const endpoint& endpoint) noexcept;

  /**
   * Shuts down socket send and/or receive operations.
   */
  [[nodiscard]] std::error_code shutdown(int how) noexcept;

  /**
   * Stores the locally bound endpoint into @p endpoint, resetting it to
   * the unspecified kind for an unnamed socket.
   */
  [[nodiscard]] std::error_code local_endpoint(
      endpoint& endpoint) const noexcept;

  /**
   * Stores the connected peer endpoint into @p endpoint, resetting it to
   * the unspecified kind for an unnamed peer.
   */
  [[nodiscard]] std::error_code remote_endpoint(
      endpoint& endpoint) const noexcept;

 private:
  native_handle_type fd_ = -1;
};

}  // namespace bnio::async_io::local

#endif  // BNIO_ASYNC_IO_LOCAL_SOCKET_VIEW_H_
