#pragma once
#ifndef BNIO_UDP_SOCKET_H_
#define BNIO_UDP_SOCKET_H_

#include <bnio/async_io/socket_view.h>
#include <bnio/buffer.h>
#include <bnio/export.h>
#include <bnio/ip.h>
#include <sys/socket.h>

#include <cstddef>
#include <system_error>

namespace bnio::udp {

/**
 * Move-only RAII owner for a native UDP datagram socket.
 */
class BNIO_EXPORT socket {
 public:
  using native_handle_type = async_io::datagram_socket_view::native_handle_type;

  socket() noexcept = default;
  explicit socket(native_handle_type fd) noexcept : fd_(fd) {}
  ~socket() noexcept;

  socket(const socket&) = delete;
  socket& operator=(const socket&) = delete;
  socket(socket&& other) noexcept;
  socket& operator=(socket&& other) noexcept;

  [[nodiscard]] native_handle_type native_handle() const noexcept {
    return fd_;
  }
  [[nodiscard]] native_handle_type get_native_handle() const noexcept {
    return native_handle();
  }
  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }
  [[nodiscard]] async_io::datagram_socket_view view() const noexcept {
    return async_io::datagram_socket_view(fd_);
  }

  [[nodiscard]] std::error_code open(int family = AF_INET) noexcept;
  [[nodiscard]] std::error_code open(ip::udp protocol) noexcept;
  [[nodiscard]] std::error_code bind(const ip::endpoint& endpoint) noexcept;

  /** Sets the default peer used by send/receive operations. */
  [[nodiscard]] std::error_code connect(const ip::endpoint& endpoint) noexcept;

  [[nodiscard]] std::error_code close() noexcept;
  [[nodiscard]] native_handle_type release() noexcept;
  void assign(native_handle_type fd) noexcept;
  [[nodiscard]] std::error_code shutdown(int how) noexcept;
  [[nodiscard]] std::error_code set_reuse_address(bool enabled) noexcept;
  [[nodiscard]] std::error_code local_endpoint(
      ip::endpoint& endpoint) const noexcept;
  [[nodiscard]] std::error_code remote_endpoint(
      ip::endpoint& endpoint) const noexcept;

  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_send(Scheduler scheduler, Buffer&& buffer,
                                int flags = 0);

  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_receive(Scheduler scheduler, Buffer&& buffer,
                                   int flags = 0);

  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_send_to(Scheduler scheduler, Buffer&& buffer,
                                   const ip::endpoint& endpoint, int flags = 0);

  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_receive_from(Scheduler scheduler, Buffer&& buffer,
                                        ip::endpoint& endpoint, int flags = 0);

 private:
  native_handle_type fd_ = -1;
};

}  // namespace bnio::udp

#endif  // BNIO_UDP_SOCKET_H_
