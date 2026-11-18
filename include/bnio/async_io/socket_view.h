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
 * Non-owning view over a native socket descriptor; copying copies only the
 * descriptor value. The caller owns and closes the socket, and the view is
 * valid only while its owner exists.
 */
class BNIO_EXPORT socket_view {
 public:
  /** Native socket descriptor type. */
  using native_handle_type = int;

  /** Creates an invalid socket view. */
  constexpr socket_view() noexcept = default;

  /** Wraps a native descriptor without taking ownership. */
  constexpr explicit socket_view(native_handle_type fd) noexcept : fd_(fd) {}

  /** Copies the view without taking ownership. */
  constexpr socket_view(const socket_view&) noexcept = default;

  /** Copies the view without taking ownership. */
  constexpr socket_view& operator=(const socket_view&) noexcept = default;

  /** Moves the view by copying the descriptor value. */
  constexpr socket_view(socket_view&&) noexcept = default;

  /** Moves the view by copying the descriptor value. */
  constexpr socket_view& operator=(socket_view&&) noexcept = default;

  /** Destroys the view without closing the socket. */
  ~socket_view() noexcept = default;

  /** Returns the wrapped native socket descriptor. */
  [[nodiscard]] constexpr native_handle_type native_handle() const noexcept {
    return fd_;
  }

  /** Returns whether the view references a valid descriptor value. */
  [[nodiscard]] constexpr bool valid() const noexcept { return fd_ >= 0; }

 private:
  native_handle_type fd_ = -1;
};

/**
 * Non-owning datagram socket view; datagram transfer is async-only, so stream
 * partial-I/O rules do not apply. The caller owns the socket.
 */
class BNIO_EXPORT datagram_socket_view {
 public:
  /** Native socket descriptor type. */
  using native_handle_type = socket_view::native_handle_type;

  /** Creates an invalid datagram socket view. */
  constexpr datagram_socket_view() noexcept = default;

  /** Wraps a native descriptor without taking ownership. */
  constexpr explicit datagram_socket_view(native_handle_type fd) noexcept
      : socket_(fd) {}

  /** Wraps a generic socket view as a datagram socket view. */
  constexpr explicit datagram_socket_view(socket_view socket) noexcept
      : socket_(socket) {}

  /** Returns the wrapped native socket descriptor. */
  [[nodiscard]] constexpr native_handle_type native_handle() const noexcept {
    return socket_.native_handle();
  }

  /** Returns whether the view references a valid descriptor value. */
  [[nodiscard]] constexpr bool valid() const noexcept {
    return socket_.valid();
  }

  /** Binds the socket to an IP endpoint. */
  [[nodiscard]] std::error_code bind(const ip::endpoint& endpoint) noexcept;

  /** Sets the default peer used by subsequent send and receive calls. */
  [[nodiscard]] std::error_code connect(const ip::endpoint& endpoint) noexcept;

  /** Shuts down socket send and/or receive operations. */
  [[nodiscard]] std::error_code shutdown(int how) noexcept;

  /** Enables or disables address reuse on the socket. */
  [[nodiscard]] std::error_code set_reuse_address(bool enabled) noexcept;

  /** Stores the locally bound endpoint into @p endpoint. */
  [[nodiscard]] std::error_code local_endpoint(
      ip::endpoint& endpoint) const noexcept;

  /** Stores the connected peer endpoint into @p endpoint. */
  [[nodiscard]] std::error_code remote_endpoint(
      ip::endpoint& endpoint) const noexcept;

 private:
  socket_view socket_;
};

/**
 * Non-owning view over a stream socket in any lifecycle state (unbound, bound,
 * listening, connected); stream transfer is async-only. The caller owns the
 * socket and must outlive the view.
 */
class BNIO_EXPORT stream_socket_view {
 public:
  /** Native socket descriptor type. */
  using native_handle_type = socket_view::native_handle_type;

  /** Creates an invalid stream socket view. */
  constexpr stream_socket_view() noexcept = default;

  /** Wraps a native descriptor without taking ownership. */
  constexpr explicit stream_socket_view(native_handle_type fd) noexcept
      : socket_(fd) {}

  /** Wraps a generic socket view as a stream socket view. */
  constexpr explicit stream_socket_view(socket_view socket) noexcept
      : socket_(socket) {}

  /** Copies the view without taking ownership. */
  constexpr stream_socket_view(const stream_socket_view&) noexcept = default;

  /** Copies the view without taking ownership. */
  constexpr stream_socket_view& operator=(const stream_socket_view&) noexcept =
      default;

  /** Moves the view by copying the descriptor value. */
  constexpr stream_socket_view(stream_socket_view&&) noexcept = default;

  /** Moves the view by copying the descriptor value. */
  constexpr stream_socket_view& operator=(stream_socket_view&&) noexcept =
      default;

  /** Destroys the view without closing the socket. */
  ~stream_socket_view() noexcept = default;

  /** Returns the wrapped native socket descriptor. */
  [[nodiscard]] constexpr native_handle_type native_handle() const noexcept {
    return socket_.native_handle();
  }

  /** Returns whether the view references a valid descriptor value. */
  [[nodiscard]] constexpr bool valid() const noexcept {
    return socket_.valid();
  }

  /** Binds the socket to an IP endpoint. */
  [[nodiscard]] std::error_code bind(const ip::endpoint& endpoint) noexcept;

  /** Marks the bound stream socket as listening. */
  [[nodiscard]] std::error_code listen(int backlog) noexcept;

  /** Connects the socket to an IP endpoint. */
  [[nodiscard]] std::error_code connect(const ip::endpoint& endpoint) noexcept;

  /** Shuts down socket send and/or receive operations. */
  [[nodiscard]] std::error_code shutdown(int how) noexcept;

  /** Enables or disables address reuse on the socket. */
  [[nodiscard]] std::error_code set_reuse_address(bool enabled) noexcept;

  /** Stores the connected peer endpoint into @p endpoint. */
  [[nodiscard]] std::error_code remote_endpoint(
      ip::endpoint& endpoint) const noexcept;

 private:
  socket_view socket_;
};

}  // namespace bnio::async_io

#endif  // BNIO_ASYNC_IO_SOCKET_VIEW_H_
