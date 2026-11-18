/**
 * @file socket_address.h
 * @brief Sockaddr storage wrapper for kqueue.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_SOCKET_ADDRESS_H_
#define BNIO_ASYNC_IO_BSD_SOCKET_ADDRESS_H_

#include <bnio/async_io/ip/endpoint.h>
#include <bnio/async_io/local/endpoint.h>
#include <bnio/export.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <optional>

namespace bnio::async_io::bsd_native {

/** Native sockaddr storage for an IP or local endpoint. */
class BNIO_EXPORT socket_address {
 public:
  /** Creates an empty address that holds no endpoint. */
  socket_address() noexcept;

  /** Stores @p endpoint as a native sockaddr value. */
  explicit socket_address(const ip::endpoint& endpoint) noexcept;

  /**
   * Stores a local endpoint as a native sockaddr_un value. An endpoint
   * with no representable address (unspecified kind) leaves the storage
   * empty.
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
   *  when empty. */
  [[nodiscard]] int family() const noexcept;

  /** Returns the native sockaddr, or nullptr when no endpoint was stored. */
  [[nodiscard]] sockaddr* data() noexcept;

  /** Returns the native sockaddr, or nullptr when no endpoint was stored. */
  [[nodiscard]] const sockaddr* data() const noexcept;

  /** Returns the byte length to pass with data(), or zero when empty. */
  [[nodiscard]] socklen_t size() const noexcept;

 private:
  sockaddr_storage storage_{};
  socklen_t size_ = 0;
};

/**
 * Converts a native sockaddr into an IP endpoint.
 *
 * @param[in] address Native socket address bytes.
 * @param[in] size    Byte length of @p address.
 * @return The endpoint, or std::nullopt when @p address is null, too short
 *         for its family, or not AF_INET/AF_INET6.
 */
[[nodiscard]] BNIO_EXPORT std::optional<ip::endpoint> make_endpoint(
    const sockaddr* address, socklen_t size) noexcept;

/**
 * Converts a native sockaddr into a local (AF_UNIX) endpoint; BSD yields only
 * path-name or unnamed endpoints.
 *
 * @return std::nullopt when @p address is null, a non-zero size is shorter
 *         than its family, or the family is not AF_UNIX; a non-null address
 *         with no path bytes decodes to an unspecified endpoint.
 */
[[nodiscard]] BNIO_EXPORT std::optional<local::endpoint> make_local_endpoint(
    const sockaddr* address, socklen_t size) noexcept;

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_SOCKET_ADDRESS_H_
