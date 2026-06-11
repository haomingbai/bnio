#pragma once
#ifndef BUPP_ASYNC_IO_SOCKET_VIEW_H_
#define BUPP_ASYNC_IO_SOCKET_VIEW_H_

#include <bupp/async_io/ip/endpoint.h>
#include <bupp/export.h>

#include <system_error>

namespace bupp::async_io {

/**
 * Non-owning view over a native socket descriptor with no role-specific API.
 *
 * Copying or moving this view copies only the descriptor value. The socket
 * remains owned and closed by the caller.
 */
class BUPP_EXPORT socket_view {
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
 * Non-owning view over a socket used for bind, listen, and accept.
 *
 * Copying or moving this view copies only the descriptor value. The socket
 * remains owned and closed by the caller.
 */
class BUPP_EXPORT listening_socket_view {
 public:
  /**
   * Native socket descriptor type.
   */
  using native_handle_type = socket_view::native_handle_type;

  /**
   * Creates an invalid listening socket view.
   */
  constexpr listening_socket_view() noexcept = default;

  /**
   * Wraps a native socket descriptor without taking ownership.
   */
  constexpr explicit listening_socket_view(native_handle_type fd) noexcept
      : socket_(fd) {}

  /**
   * Wraps a generic socket descriptor view as a listening socket view.
   */
  constexpr explicit listening_socket_view(socket_view socket) noexcept
      : socket_(socket) {}

  /**
   * Copies a listening socket view without taking ownership.
   */
  constexpr listening_socket_view(const listening_socket_view&) noexcept =
      default;

  /**
   * Copies a listening socket view without taking ownership.
   */
  constexpr listening_socket_view& operator=(
      const listening_socket_view&) noexcept = default;

  /**
   * Moves a listening socket view by copying the descriptor value.
   */
  constexpr listening_socket_view(listening_socket_view&&) noexcept = default;

  /**
   * Moves a listening socket view by copying the descriptor value.
   */
  constexpr listening_socket_view& operator=(listening_socket_view&&) noexcept =
      default;

  /**
   * Destroys the view without closing the socket.
   */
  ~listening_socket_view() noexcept = default;

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
   *
   * @see bind
   */
  [[nodiscard]] std::error_code bind(const ip::endpoint& endpoint) noexcept;

  /**
   * Marks the socket as a listening socket.
   *
   * @see listen
   */
  [[nodiscard]] std::error_code listen(int backlog) noexcept;

  /**
   * Shuts down socket send and/or receive operations.
   *
   * @see shutdown
   */
  [[nodiscard]] std::error_code shutdown(int how) noexcept;

  /**
   * Enables or disables address reuse on the socket.
   *
   * @see setsockopt
   */
  [[nodiscard]] std::error_code set_reuse_address(bool enabled) noexcept;

 private:
  socket_view socket_;
};

/**
 * Non-owning view over a socket used for connect, send, and receive.
 *
 * Copying or moving this view copies only the descriptor value. The socket
 * remains owned and closed by the caller.
 */
class BUPP_EXPORT stream_socket_view {
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
   * Connects the socket to an IP endpoint.
   *
   * @see connect
   */
  [[nodiscard]] std::error_code connect(const ip::endpoint& endpoint) noexcept;

  /**
   * Shuts down socket send and/or receive operations.
   *
   * @see shutdown
   */
  [[nodiscard]] std::error_code shutdown(int how) noexcept;

  /**
   * Enables or disables address reuse on the socket.
   *
   * @see setsockopt
   */
  [[nodiscard]] std::error_code set_reuse_address(bool enabled) noexcept;

 private:
  socket_view socket_;
};

}  // namespace bupp::async_io

#endif  // BUPP_ASYNC_IO_SOCKET_VIEW_H_
