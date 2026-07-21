/**
 * @file socket_address.h
 * @brief Sockaddr storage wrapper for kqueue.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_SOCKET_ADDRESS_H_
#define BNIO_ASYNC_IO_BSD_SOCKET_ADDRESS_H_

#include <bnio/async_io/ip/endpoint.h>
#include <bnio/export.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <optional>

namespace bnio::async_io::bsd_native {

/** BSD-native socket address storage for an IP endpoint. */
class BNIO_EXPORT socket_address {
 public:
  socket_address() noexcept;
  explicit socket_address(const ip::endpoint& endpoint) noexcept;

  socket_address(const socket_address&) noexcept = default;
  socket_address& operator=(const socket_address&) noexcept = default;
  socket_address(socket_address&&) noexcept = default;
  socket_address& operator=(socket_address&&) noexcept = default;
  ~socket_address() noexcept = default;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] int family() const noexcept;
  [[nodiscard]] sockaddr* data() noexcept;
  [[nodiscard]] const sockaddr* data() const noexcept;
  [[nodiscard]] socklen_t size() const noexcept;

 private:
  sockaddr_storage storage_{};
  socklen_t size_ = 0;
};

[[nodiscard]] BNIO_EXPORT std::optional<ip::endpoint> make_endpoint(
    const sockaddr* address, socklen_t size) noexcept;

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_SOCKET_ADDRESS_H_
