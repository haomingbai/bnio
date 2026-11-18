/**
 * @file socket_address.h
 * @brief Sockaddr storage wrapper for io_uring.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_SOCKET_ADDRESS_H_
#define BNIO_ASYNC_IO_LINUX_SOCKET_ADDRESS_H_

#include <bnio/async_io/ip/endpoint.h>
#include <bnio/async_io/local/endpoint.h>
#include <bnio/export.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <optional>

namespace bnio::async_io::linux_native {

/** Linux-native socket address storage for an IP endpoint. */
class BNIO_EXPORT socket_address {
 public:
  /** Creates an empty address that holds no endpoint. */
  socket_address() noexcept;

  /** Stores @p endpoint as a native sockaddr value. */
  explicit socket_address(const ip::endpoint& endpoint) noexcept;

  /**
   * Stores a local endpoint as a native sockaddr_un value; an endpoint with
   * no representable address leaves the storage empty.
   */
  explicit socket_address(const local::endpoint& endpoint) noexcept;

  /** Copies the stored address bytes. */
  socket_address(const socket_address&) noexcept = default;

  /** Copies the stored address bytes. */
  socket_address& operator=(const socket_address&) noexcept = default;

  /** Moves the stored address bytes. */
  socket_address(socket_address&&) noexcept = default;

  /** Moves the stored address bytes. */
  socket_address& operator=(socket_address&&) noexcept = default;

  /** Destroys the address storage. */
  ~socket_address() noexcept = default;

  /** Returns whether an endpoint was stored. */
  [[nodiscard]] bool valid() const noexcept;

  /** Returns the stored family (AF_INET, AF_INET6, or AF_UNIX), or AF_UNSPEC
   *  if empty. */
  [[nodiscard]] int family() const noexcept;

  /** Native sockaddr pointer for socket calls; nullptr when empty. */
  [[nodiscard]] sockaddr* data() noexcept;

  /** Native sockaddr pointer for socket calls; nullptr when empty. */
  [[nodiscard]] const sockaddr* data() const noexcept;

  /** Byte length to pass with data() to socket calls; zero when empty. */
  [[nodiscard]] socklen_t size() const noexcept;

 private:
  sockaddr_storage storage_{};
  socklen_t size_ = 0;
};

/**
 * Returns the IP endpoint encoded in @p address, or std::nullopt when it is
 * null, short for its claimed family, or not AF_INET/AF_INET6.
 */
[[nodiscard]] BNIO_EXPORT std::optional<ip::endpoint> make_endpoint(
    const sockaddr* address, socklen_t size) noexcept;

/**
 * Returns the AF_UNIX endpoint encoded in @p address, or std::nullopt when it
 * is null, short for its claimed family, or not AF_UNIX. An address of exactly
 * the family field size is an unnamed socket.
 */
[[nodiscard]] BNIO_EXPORT std::optional<local::endpoint> make_local_endpoint(
    const sockaddr* address, socklen_t size) noexcept;

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_SOCKET_ADDRESS_H_
