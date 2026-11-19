/**
 * @file socket.h
 * @brief RAII UDP datagram socket owner.
 */

#pragma once
#ifndef BNIO_UDP_SOCKET_H_
#define BNIO_UDP_SOCKET_H_

#include <bnio/async_io/socket_view.h>
#include <bnio/buffer.h>
#include <bnio/export.h>
#include <bnio/ip.h>
#include <fcntl.h>
#include <sys/socket.h>

#include <cstddef>
#include <system_error>

namespace bnio::udp {

/**
 * Move-only RAII owner for a native UDP datagram socket.
 *
 * Every async operation transfers exactly one datagram; no operation retries
 * with write-all/read-all. connect() sets the default peer for
 * async_send()/async_receive(); async_send_to()/async_receive_from()
 * address each datagram explicitly.
 */
class BNIO_EXPORT socket {
 public:
  /**
   * Native datagram socket descriptor type.
   */
  using native_handle_type = async_io::datagram_socket_view::native_handle_type;

  /**
   * Creates a closed socket owner.
   */
  socket() noexcept = default;

  /**
   * Takes ownership of @p fd and sets it non-blocking, so the kqueue backend
   * can skip per-operation fcntl calls.
   */
  explicit socket(native_handle_type fd) noexcept : fd_(fd) {
    if (fd_ >= 0) {
      const int flags = ::fcntl(fd_, F_GETFL, 0);
      if (flags >= 0 && (flags & O_NONBLOCK) == 0) {
        (void)::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
      }
    }
  }
  ~socket() noexcept;

  socket(const socket&) = delete;
  socket& operator=(const socket&) = delete;

  /**
   * Moves descriptor ownership from @p other, leaving @p other closed.
   */
  socket(socket&& other) noexcept;

  /**
   * Moves descriptor ownership from @p other, closing any descriptor this
   * socket held; @p other is left closed.
   */
  socket& operator=(socket&& other) noexcept;

  /**
   * Returns the owned native descriptor, or -1 when closed.
   */
  [[nodiscard]] native_handle_type native_handle() const noexcept {
    return fd_;
  }

  /**
   * Alias of native_handle().
   */
  [[nodiscard]] native_handle_type get_native_handle() const noexcept {
    return native_handle();
  }

  /**
   * Returns whether the socket holds an open descriptor.
   */
  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

  /**
   * Returns a non-owning datagram socket view of this socket.
   */
  [[nodiscard]] async_io::datagram_socket_view view() const noexcept {
    return async_io::datagram_socket_view(fd_);
  }

  /**
   * Opens a UDP socket for @p family (e.g. AF_INET); an already open socket
   * is left unchanged.
   *
   * @return Empty on success, otherwise the errno that failed creation.
   */
  [[nodiscard]] std::error_code open(int family = AF_INET) noexcept;

  /**
   * Opens a UDP socket for @p protocol's IP version.
   *
   * @return Empty on success, EAFNOSUPPORT when @p protocol has no IP
   *         version, or the errno that failed creation.
   */
  [[nodiscard]] std::error_code open(ip::udp protocol) noexcept;

  /**
   * Binds the socket to an IP endpoint.
   */
  [[nodiscard]] std::error_code bind(const ip::endpoint& endpoint) noexcept;

  /** Sets the default peer used by send/receive operations. */
  [[nodiscard]] std::error_code connect(const ip::endpoint& endpoint) noexcept;

  /**
   * Closes the socket and returns any ::close error; the socket is left
   * closed either way.
   *
   * Does not cancel operations already in flight; they complete normally.
   */
  [[nodiscard]] std::error_code close() noexcept;

  /**
   * Releases descriptor ownership to the caller, leaving this socket closed.
   *
   * @return The previous descriptor, or -1 when the socket was not open.
   */
  [[nodiscard]] native_handle_type release() noexcept;

  /**
   * Takes ownership of @p fd, closing any descriptor currently held, and
   * sets it non-blocking like the owning constructor.
   */
  void assign(native_handle_type fd) noexcept;

  /**
   * Shuts down socket send and/or receive operations.
   */
  [[nodiscard]] std::error_code shutdown(int how) noexcept;

  /**
   * Enables or disables address reuse on the socket.
   */
  [[nodiscard]] std::error_code set_reuse_address(bool enabled) noexcept;

  /**
   * Stores the locally bound endpoint into @p endpoint.
   */
  [[nodiscard]] std::error_code local_endpoint(
      ip::endpoint& endpoint) const noexcept;

  /**
   * Stores the connected peer endpoint into @p endpoint.
   */
  [[nodiscard]] std::error_code remote_endpoint(
      ip::endpoint& endpoint) const noexcept;

  /**
   * Sends exactly one datagram from @p buffer to the default peer set by
   * connect(). @p buffer must stay valid until completion.
   *
   * Completion delivers set_value(std::error_code, bytes) or set_stopped().
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_send(Scheduler scheduler, Buffer&& buffer,
                                int flags = 0);

  /**
   * Receives exactly one datagram into @p buffer from the default peer set
   * by connect(). @p buffer must stay valid until completion.
   *
   * Completion delivers set_value(std::error_code, bytes) or set_stopped().
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_receive(Scheduler scheduler, Buffer&& buffer,
                                   int flags = 0);

  /**
   * Sends exactly one datagram from @p buffer to @p endpoint without
   * requiring a connected socket; @p buffer must stay valid until
   * completion.
   *
   * Completion delivers set_value(std::error_code, bytes) or set_stopped().
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_send_to(Scheduler scheduler, Buffer&& buffer,
                                   const ip::endpoint& endpoint, int flags = 0);

  /**
   * Receives one datagram into @p buffer and writes the source endpoint to
   * @p endpoint. Both must stay alive until completion.
   *
   * Completion delivers set_value(std::error_code, bytes) or set_stopped().
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_receive_from(Scheduler scheduler, Buffer&& buffer,
                                        ip::endpoint& endpoint, int flags = 0);

 private:
  native_handle_type fd_ = -1;
};

}  // namespace bnio::udp

#endif  // BNIO_UDP_SOCKET_H_
