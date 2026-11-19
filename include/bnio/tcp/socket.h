/**
 * @file socket.h
 * @brief RAII TCP stream socket owner.
 */

#pragma once
#ifndef BNIO_TCP_SOCKET_H_
#define BNIO_TCP_SOCKET_H_

#include <bnio/async_io/socket_view.h>
#include <bnio/buffer.h>
#include <bnio/export.h>
#include <bnio/ip.h>
#include <fcntl.h>
#include <sys/socket.h>

#include <system_error>
#include <utility>

namespace bnio::tcp {

/**
 * RAII owner of a native TCP stream socket descriptor. Move-only; closes the
 * descriptor on destruction. view() returns a non-owning view.
 */
class BNIO_EXPORT socket {
 public:
  /**
   * Native stream socket descriptor type.
   */
  using native_handle_type = async_io::stream_socket_view::native_handle_type;

  /**
   * Creates a closed socket owner.
   */
  socket() noexcept = default;

  /**
   * Takes ownership of @p fd and sets it non-blocking, so the kqueue backend
   * needs no per-operation fcntl.
   */
  explicit socket(native_handle_type fd) noexcept : fd_(fd) {
    if (fd_ >= 0) {
      const int flags = ::fcntl(fd_, F_GETFL, 0);
      if (flags >= 0 && (flags & O_NONBLOCK) == 0) {
        (void)::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
      }
    }
  }

  /**
   * Closes the owned descriptor, if any.
   */
  ~socket() noexcept;

  /**
   * Copy construction is disabled because the socket owns a descriptor.
   */
  socket(const socket&) = delete;

  /**
   * Copy assignment is disabled because the socket owns a descriptor.
   */
  socket& operator=(const socket&) = delete;

  /**
   * Moves descriptor ownership from another socket.
   */
  socket(socket&& other) noexcept;

  /** Closes the current descriptor, then takes ownership from @p other. */
  socket& operator=(socket&& other) noexcept;

  /**
   * Returns the owned native descriptor, or -1 when closed.
   */
  [[nodiscard]] native_handle_type native_handle() const noexcept {
    return fd_;
  }

  /** Alias for native_handle(). */
  [[nodiscard]] native_handle_type get_native_handle() const noexcept {
    return native_handle();
  }

  /** True when a descriptor is owned. */
  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

  /**
   * Returns a non-owning view of the owned descriptor.
   */
  [[nodiscard]] async_io::stream_socket_view view() const noexcept {
    return async_io::stream_socket_view(fd_);
  }

  /**
   * Returns this stream's next layer.
   */
  [[nodiscard]] socket& next_layer() noexcept { return *this; }

  /**
   * Returns this stream's next layer.
   */
  [[nodiscard]] const socket& next_layer() const noexcept { return *this; }

  /**
   * Returns this stream's lowest layer.
   */
  [[nodiscard]] socket& lowest_layer() noexcept { return *this; }

  /**
   * Returns this stream's lowest layer.
   */
  [[nodiscard]] const socket& lowest_layer() const noexcept { return *this; }

  /**
   * Read-all sender: repeats until the buffer is full; completes short only
   * on EOF/error.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_read(Scheduler scheduler, Buffer&& buffer,
                                int flags = 0);

  /** Sender for one read attempt; explicit read-some spelling. */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_read_some(Scheduler scheduler, Buffer&& buffer,
                                     int flags = 0);

  /**
   * Write-all sender: retries short writes until complete, error, or stop.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_write(Scheduler scheduler, Buffer&& buffer,
                                 int flags = 0);

  /** One write attempt; short writes are not retried. */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_write_some(Scheduler scheduler, Buffer&& buffer,
                                      int flags = 0);

  /** Connect sender for @p endpoint via the queued submission path. */
  template <class Scheduler>
  [[nodiscard]] auto async_connect(Scheduler scheduler,
                                   const ip::endpoint& endpoint);

  /**
   * Opens a TCP stream socket for an address family.
   */
  [[nodiscard]] std::error_code open(int family = AF_INET) noexcept;

  /**
   * Opens a TCP stream socket for a TCP protocol tag.
   */
  [[nodiscard]] std::error_code open(ip::tcp protocol) noexcept;

  /**
   * Closes the owned descriptor, if any.
   *
   * Does not cancel operations already in flight; they complete normally.
   */
  [[nodiscard]] std::error_code close() noexcept;

  /**
   * Releases ownership and returns the native descriptor.
   */
  [[nodiscard]] native_handle_type release() noexcept;

  /**
   * Replaces the owned descriptor, closing the old descriptor if needed.
   */
  void assign(native_handle_type fd) noexcept;

  /**
   * Binds the socket to an IP endpoint.
   */
  [[nodiscard]] std::error_code bind(const ip::endpoint& endpoint) noexcept;

  /** Shuts down socket send and/or receive operations. */
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

 private:
  native_handle_type fd_ = -1;
};

}  // namespace bnio::tcp

#endif  // BNIO_TCP_SOCKET_H_
