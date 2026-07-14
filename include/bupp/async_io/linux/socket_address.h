#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_SOCKET_ADDRESS_H_
#define BUPP_ASYNC_IO_LINUX_SOCKET_ADDRESS_H_

#include <bupp/async_io/ip/endpoint.h>
#include <bupp/export.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <optional>

namespace bupp::async_io::linux_native {

/** Linux-native socket address storage for an IP endpoint. */
class BUPP_EXPORT socket_address {
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

[[nodiscard]] BUPP_EXPORT std::optional<ip::endpoint> make_endpoint(
    const sockaddr* address, socklen_t size) noexcept;

}  // namespace bupp::async_io::linux_native

#endif  // BUPP_ASYNC_IO_LINUX_SOCKET_ADDRESS_H_
