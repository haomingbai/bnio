/**
 * @file socket_view.h
 * @brief Non-owning views (stream_socket_view, datagram_socket_view).
 */

#pragma once
#ifndef BNIO_ASYNC_IO_SOCKET_VIEW_H_
#define BNIO_ASYNC_IO_SOCKET_VIEW_H_

#include <bnio/async_io/ip/endpoint.h>
#include <bnio/export.h>

#include <system_error>

namespace bnio::async_io {

/**
 * Non-owning view over a native socket descriptor with no role-specific API.
 *
 * Copying or moving this view copies only the descriptor value. The socket
 * remains owned and closed by the caller.
 */
class BNIO_EXPORT socket_view {
 public:
  /**
   * Native socket descriptor type.
   */
  using native_handle_type = int;

  /**
   * Creates an invalid socket view.
   */
  constexpr socket_view() noexcept = default;

  /**
   * Wraps a native socket descriptor without taking ownership.
   */
  constexpr explicit socket_view(native_handle_type fd) noexcept : fd_(fd) {}

  /**
   * Copies a socket view without taking ownership.
   */
  constexpr socket_view(const socket_view&) noexcept = default;

  /**
   * Copies a socket view without taking ownership.
   */
  constexpr socket_view& operator=(const socket_view&) noexcept = default;

  /**
   * Moves a socket view by copying the descriptor value.
   */
  constexpr socket_view(socket_view&&) noexcept = default;

  /**
   * Moves a socket view by copying the descriptor value.
   */
  constexpr socket_view& operator=(socket_view&&) noexcept = default;

  /**
   * Destroys the view without closing the socket.
   */
  ~socket_view() noexcept = default;

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

 private:
  native_handle_type fd_ = -1;
};

/**
 * Non-owning view over a connectionless or connected datagram socket.
 */
class BNIO_EXPORT datagram_socket_view {
 public:
  using native_handle_type = socket_view::native_handle_type;

  constexpr datagram_socket_view() noexcept = default;
  constexpr explicit datagram_socket_view(native_handle_type fd) noexcept
      : socket_(fd) {}
  constexpr explicit datagram_socket_view(socket_view socket) noexcept
      : socket_(socket) {}

  [[nodiscard]] constexpr native_handle_type native_handle() const noexcept {
    return socket_.native_handle();
  }

  [[nodiscard]] constexpr bool valid() const noexcept {
    return socket_.valid();
  }

  [[nodiscard]] std::error_code bind(const ip::endpoint& endpoint) noexcept;
  [[nodiscard]] std::error_code connect(const ip::endpoint& endpoint) noexcept;
  [[nodiscard]] std::error_code shutdown(int how) noexcept;
  [[nodiscard]] std::error_code set_reuse_address(bool enabled) noexcept;

  [[nodiscard]] std::error_code local_endpoint(
      ip::endpoint& endpoint) const noexcept;
  [[nodiscard]] std::error_code remote_endpoint(
      ip::endpoint& endpoint) const noexcept;

 private:
  socket_view socket_;
};

/**
 * Non-owning view over a stream socket in any lifecycle state.
 *
 * Copying or moving this view copies only the descriptor value. The socket
 * remains owned and closed by the caller.
 */
class BNIO_EXPORT stream_socket_view {
 public:
  /**
   * Native socket descriptor type.
   */
  using native_handle_type = socket_view::native_handle_type;

  /**
   * Creates an invalid stream socket view.
   */
  constexpr stream_socket_view() noexcept = default;

  /**
   * Wraps a native socket descriptor without taking ownership.
   */
  constexpr explicit stream_socket_view(native_handle_type fd) noexcept
      : socket_(fd) {}

  /**
   * Wraps a generic socket descriptor view as a stream socket view.
   */
  constexpr explicit stream_socket_view(socket_view socket) noexcept
      : socket_(socket) {}

  /**
   * Copies a stream socket view without taking ownership.
   */
  constexpr stream_socket_view(const stream_socket_view&) noexcept = default;

  /**
   * Copies a stream socket view without taking ownership.
   */
  constexpr stream_socket_view& operator=(const stream_socket_view&) noexcept =
      default;

  /**
   * Moves a stream socket view by copying the descriptor value.
   */
  constexpr stream_socket_view(stream_socket_view&&) noexcept = default;

  /**
   * Moves a stream socket view by copying the descriptor value.
   */
  constexpr stream_socket_view& operator=(stream_socket_view&&) noexcept =
      default;

  /**
   * Destroys the view without closing the socket.
   */
  ~stream_socket_view() noexcept = default;

  /**
   * Returns the wrapped native socket descriptor.
   */
  [[nodiscard]] constexpr native_handle_type native_handle() const noexcept {
    return socket_.native_handle();
  }

  /**
   * Returns whether this view references a valid descriptor value.
   */
  [[nodiscard]] constexpr bool valid() const noexcept {
    return socket_.valid();
  }

  /**
   * Binds the socket to an IP endpoint.
   */
  [[nodiscard]] std::error_code bind(const ip::endpoint& endpoint) noexcept;

  /**
   * Marks the bound stream socket as listening.
   */
  [[nodiscard]] std::error_code listen(int backlog) noexcept;

  /**
   * Connects the socket to an IP endpoint.
   */
  [[nodiscard]] std::error_code connect(const ip::endpoint& endpoint) noexcept;

  /**
   * Shuts down socket send and/or receive operations.
   */
  [[nodiscard]] std::error_code shutdown(int how) noexcept;

  /**
   * Enables or disables address reuse on the socket.
   */
  [[nodiscard]] std::error_code set_reuse_address(bool enabled) noexcept;

 private:
  socket_view socket_;
};

}  // namespace bnio::async_io

#endif  // BNIO_ASYNC_IO_SOCKET_VIEW_H_
