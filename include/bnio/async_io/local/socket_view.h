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
 * Sync: bind(), listen(), connect(), shutdown(), endpoint queries; transfer is
 * async-only. Covers unbound, bound, listening, and connected states (listening
 * is a state, not a separate kind); caller owns and closes.
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
   * Whether the descriptor value is non-negative.
   */
  [[nodiscard]] constexpr bool valid() const noexcept { return fd_ >= 0; }

  /**
   * Binds to @p endpoint; a non-convertible endpoint reports
   * std::errc::invalid_argument without a syscall.
   */
  [[nodiscard]] std::error_code bind(const endpoint& endpoint) noexcept;

  /**
   * Marks the bound stream socket as listening.
   */
  [[nodiscard]] std::error_code listen(int backlog) noexcept;

  /**
   * Connects to @p endpoint; a non-convertible endpoint reports
   * std::errc::invalid_argument without a syscall.
   */
  [[nodiscard]] std::error_code connect(const endpoint& endpoint) noexcept;

  /**
   * Shuts down socket send and/or receive operations.
   */
  [[nodiscard]] std::error_code shutdown(int how) noexcept;

  /**
   * Stores the peer endpoint; unspecified kind for an unnamed peer.
   */
  [[nodiscard]] std::error_code remote_endpoint(
      endpoint& endpoint) const noexcept;

 private:
  native_handle_type fd_ = -1;
};

/**
 * Non-owning view over a connectionless or connected local datagram socket.
 *
 * Kept distinct from stream views so stream partial-I/O rules cannot apply.
 * The caller keeps ownership and closes the socket.
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
   * Whether the descriptor value is non-negative.
   */
  [[nodiscard]] constexpr bool valid() const noexcept { return fd_ >= 0; }

  /**
   * Binds to @p endpoint; a non-convertible endpoint reports
   * std::errc::invalid_argument without a syscall.
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
   * Stores the locally bound endpoint; unspecified kind if unnamed.
   */
  [[nodiscard]] std::error_code local_endpoint(
      endpoint& endpoint) const noexcept;

  /**
   * Stores the peer endpoint; unspecified kind for an unnamed peer.
   */
  [[nodiscard]] std::error_code remote_endpoint(
      endpoint& endpoint) const noexcept;

 private:
  native_handle_type fd_ = -1;
};

}  // namespace bnio::async_io::local

#endif  // BNIO_ASYNC_IO_LOCAL_SOCKET_VIEW_H_
