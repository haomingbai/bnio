#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_SOCKET_ADDRESS_H_
#define BUPP_ASYNC_IO_LINUX_SOCKET_ADDRESS_H_

#include <bupp/async_io/ip/endpoint.h>
#include <bupp/export.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <optional>

namespace bupp::async_io::linux_native {

/**
 * Native socket address storage for an IP endpoint.
 *
 * The storage is an inline value and is safe to copy or move.
 */
class BUPP_EXPORT socket_address {
 public:
  /**
   * Creates an invalid socket address.
   */
  socket_address() noexcept;

  /**
   * Creates a native socket address from an IP endpoint.
   */
  explicit socket_address(const ip::endpoint& endpoint) noexcept;

  /**
   * Copies native socket address storage.
   */
  socket_address(const socket_address&) noexcept = default;

  /**
   * Copies native socket address storage.
   */
  socket_address& operator=(const socket_address&) noexcept = default;

  /**
   * Moves native socket address storage.
   */
  socket_address(socket_address&&) noexcept = default;

  /**
   * Moves native socket address storage.
   */
  socket_address& operator=(socket_address&&) noexcept = default;

  /**
   * Destroys the stored socket address value.
   */
  ~socket_address() noexcept = default;

  /**
   * Returns whether this object stores a socket address.
   */
  [[nodiscard]] bool valid() const noexcept;

  /**
   * Returns the native address family, or AF_UNSPEC when invalid.
   */
  [[nodiscard]] int family() const noexcept;

  /**
   * Returns the mutable native socket address pointer, or nullptr when invalid.
   */
  [[nodiscard]] sockaddr* data() noexcept;

  /**
   * Returns the native socket address pointer, or nullptr when invalid.
   */
  [[nodiscard]] const sockaddr* data() const noexcept;

  /**
   * Returns the native socket address byte size.
   */
  [[nodiscard]] socklen_t size() const noexcept;

 private:
  sockaddr_storage storage_{};
  socklen_t size_ = 0;
};

/**
 * Converts a native socket address into an IP endpoint when supported.
 */
[[nodiscard]] BUPP_EXPORT std::optional<ip::endpoint> make_endpoint(
    const sockaddr* address, socklen_t size) noexcept;

}  // namespace bupp::async_io::linux_native

#endif  // BUPP_ASYNC_IO_LINUX_SOCKET_ADDRESS_H_
