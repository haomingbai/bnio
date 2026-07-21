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
#include <sys/socket.h>

#include <system_error>
#include <utility>

namespace bnio::tcp {

/**
 * RAII owner for a native TCP stream socket descriptor.
 *
 * tcp_socket closes its descriptor on destruction. It is move-only because
 * duplicating ownership of one descriptor would make close semantics ambiguous.
 * Use view() when an API needs a non-owning async_io stream socket view.
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
   * Takes ownership of an existing native stream socket descriptor.
   */
  explicit socket(native_handle_type fd) noexcept : fd_(fd) {}

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

  /**
   * Closes the current descriptor and moves descriptor ownership from another
   * socket.
   */
  socket& operator=(socket&& other) noexcept;

  /**
   * Returns the owned native descriptor, or -1 when closed.
   */
  [[nodiscard]] native_handle_type native_handle() const noexcept {
    return fd_;
  }

  /**
   * Returns the owned native descriptor, or -1 when closed.
   */
  [[nodiscard]] native_handle_type get_native_handle() const noexcept {
    return native_handle();
  }

  /**
   * Returns whether this object currently owns a descriptor.
   */
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
   * Creates a sender for one read operation. The operation may complete with
   * fewer bytes than the buffer size.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_read(Scheduler scheduler, Buffer&& buffer,
                                int flags = 0);

  /**
   * Creates a sender for one read operation. This is the explicit read-some
   * spelling of async_read.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_read_some(Scheduler scheduler, Buffer&& buffer,
                                     int flags = 0);

  /**
   * Creates a sender that writes the whole buffer, retrying short writes until
   * the buffer is fully transferred or an error/stopped signal occurs.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_write(Scheduler scheduler, Buffer&& buffer,
                                 int flags = 0);

  /**
   * Creates a sender for one write operation without retrying short writes.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_write_some(Scheduler scheduler, Buffer&& buffer,
                                      int flags = 0);

  /**
   * Creates a sender that connects this socket to an endpoint through the
   * scheduler's queued submission path.
   */
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
   * Shuts down socket send and/or receive operations.
   *
   * @see shutdown
   */
  [[nodiscard]] std::error_code shutdown(int how) noexcept;

  /**
   * Enables or disables address reuse on the socket.
   */
  [[nodiscard]] std::error_code set_reuse_address(bool enabled) noexcept;

 private:
  native_handle_type fd_ = -1;
};

}  // namespace bnio::tcp

#endif  // BNIO_TCP_SOCKET_H_
