/**
 * @file socket.h
 * @brief RAII local (AF_UNIX) socket owners.
 */

#pragma once
#ifndef BNIO_LOCAL_SOCKET_H_
#define BNIO_LOCAL_SOCKET_H_

#include <bnio/async_io/local/protocol.h>
#include <bnio/async_io/local/socket_view.h>
#include <bnio/buffer.h>
#include <bnio/export.h>
#include <fcntl.h>
#include <sys/socket.h>

#include <system_error>
#include <utility>

namespace bnio::local {

/**
 * RAII owner for a native local stream socket descriptor.
 *
 * stream_socket closes its descriptor on destruction. It is move-only
 * because duplicating ownership of one descriptor would make close
 * semantics ambiguous. Use view() when an API needs a non-owning async_io
 * local stream socket view.
 */
class BNIO_EXPORT stream_socket {
 public:
  /**
   * Native stream socket descriptor type.
   */
  using native_handle_type =
      async_io::local::stream_socket_view::native_handle_type;

  /**
   * Creates a closed socket owner.
   */
  stream_socket() noexcept = default;

  /**
   * Takes ownership of an existing native stream socket descriptor.
   *
   * The underlying descriptor is set to non-blocking mode so that the
   * kqueue backend can skip per-operation fcntl calls.
   */
  explicit stream_socket(native_handle_type fd) noexcept : fd_(fd) {
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
  ~stream_socket() noexcept;

  /**
   * Copy construction is disabled because the socket owns a descriptor.
   */
  stream_socket(const stream_socket&) = delete;

  /**
   * Copy assignment is disabled because the socket owns a descriptor.
   */
  stream_socket& operator=(const stream_socket&) = delete;

  /**
   * Moves descriptor ownership from another socket.
   */
  stream_socket(stream_socket&& other) noexcept;

  /**
   * Closes the current descriptor and moves descriptor ownership from
   * another socket.
   */
  stream_socket& operator=(stream_socket&& other) noexcept;

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
  [[nodiscard]] async_io::local::stream_socket_view view() const noexcept {
    return async_io::local::stream_socket_view(fd_);
  }

  /**
   * Returns this stream's next layer.
   */
  [[nodiscard]] stream_socket& next_layer() noexcept { return *this; }

  /**
   * Returns this stream's next layer.
   */
  [[nodiscard]] const stream_socket& next_layer() const noexcept {
    return *this;
  }

  /**
   * Returns this stream's lowest layer.
   */
  [[nodiscard]] stream_socket& lowest_layer() noexcept { return *this; }

  /**
   * Returns this stream's lowest layer.
   */
  [[nodiscard]] const stream_socket& lowest_layer() const noexcept {
    return *this;
  }

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
   * Creates a sender that connects this socket to a local endpoint through
   * the scheduler's queued submission path.
   */
  template <class Scheduler>
  [[nodiscard]] auto async_connect(Scheduler scheduler,
                                   const async_io::local::endpoint& endpoint);

  /**
   * Opens a local stream socket for a local stream protocol tag.
   */
  [[nodiscard]] std::error_code open(
      async_io::local::stream_protocol protocol) noexcept;

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
   * Stores the connected peer endpoint into @p endpoint, resetting it to
   * the unspecified kind for an unnamed peer.
   */
  [[nodiscard]] std::error_code remote_endpoint(
      async_io::local::endpoint& endpoint) const noexcept;

 private:
  native_handle_type fd_ = -1;
};

/**
 * Move-only RAII owner for a native local datagram socket.
 *
 * Every datagram operation transfers exactly one datagram: senders and
 * receivers preserve message boundaries and never use stream-style
 * write-all or read-all retries. Call connect(endpoint) to set a default
 * peer, then use async_send() and async_receive(); use async_send_to()
 * and async_receive_from() to address each datagram explicitly.
 */
class BNIO_EXPORT datagram_socket {
 public:
  /**
   * Native datagram socket descriptor type.
   */
  using native_handle_type =
      async_io::local::datagram_socket_view::native_handle_type;

  /**
   * Creates a closed socket owner.
   */
  datagram_socket() noexcept = default;

  /**
   * Takes ownership of an existing native datagram socket descriptor.
   *
   * The underlying descriptor is set to non-blocking mode so that the
   * kqueue backend can skip per-operation fcntl calls.
   */
  explicit datagram_socket(native_handle_type fd) noexcept : fd_(fd) {
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
  ~datagram_socket() noexcept;

  datagram_socket(const datagram_socket&) = delete;
  datagram_socket& operator=(const datagram_socket&) = delete;

  /**
   * Moves ownership of the descriptor from @p other, leaving @p other
   * closed.
   */
  datagram_socket(datagram_socket&& other) noexcept;

  /**
   * Moves ownership of the descriptor from @p other, closing any descriptor
   * this socket held and leaving @p other closed.
   */
  datagram_socket& operator=(datagram_socket&& other) noexcept;

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
   * Returns a non-owning local datagram socket view of this socket.
   */
  [[nodiscard]] async_io::local::datagram_socket_view view() const noexcept {
    return async_io::local::datagram_socket_view(fd_);
  }

  /**
   * Opens a local datagram socket for a local datagram protocol tag.
   */
  [[nodiscard]] std::error_code open(
      async_io::local::datagram_protocol protocol) noexcept;

  /**
   * Binds the socket to a local endpoint. An endpoint that cannot be
   * converted to sockaddr_un reports std::errc::invalid_argument without
   * a syscall.
   */
  [[nodiscard]] std::error_code bind(
      const async_io::local::endpoint& endpoint) noexcept;

  /**
   * Sets the default peer used by send/receive operations.
   */
  [[nodiscard]] std::error_code connect(
      const async_io::local::endpoint& endpoint) noexcept;

  /**
   * Closes the socket and returns any error reported by ::close. The socket
   * is left closed either way.
   */
  [[nodiscard]] std::error_code close() noexcept;

  /**
   * Releases ownership of the socket descriptor to the caller, leaving this
   * socket closed.
   */
  [[nodiscard]] native_handle_type release() noexcept;

  /**
   * Takes ownership of an existing datagram socket descriptor, closing any
   * descriptor currently held.
   *
   * Like the owning constructor, the descriptor is set to non-blocking mode
   * so that the kqueue backend can skip per-operation fcntl calls.
   */
  void assign(native_handle_type fd) noexcept;

  /**
   * Shuts down socket send and/or receive operations.
   */
  [[nodiscard]] std::error_code shutdown(int how) noexcept;

  /**
   * Stores the locally bound endpoint into @p endpoint, resetting it to
   * the unspecified kind for an unnamed socket.
   */
  [[nodiscard]] std::error_code local_endpoint(
      async_io::local::endpoint& endpoint) const noexcept;

  /**
   * Stores the connected peer endpoint into @p endpoint, resetting it to
   * the unspecified kind for an unnamed peer.
   */
  [[nodiscard]] std::error_code remote_endpoint(
      async_io::local::endpoint& endpoint) const noexcept;

  /**
   * Creates a sender that transfers exactly one datagram from @p buffer to
   * the socket's default peer through @p scheduler. The default peer is
   * set with connect(endpoint).
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_send(Scheduler scheduler, Buffer&& buffer,
                                int flags = 0);

  /**
   * Creates a sender that receives exactly one datagram into @p buffer
   * from the socket's default peer through @p scheduler.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_receive(Scheduler scheduler, Buffer&& buffer,
                                   int flags = 0);

  /**
   * Creates a sender that sends one datagram from @p buffer to @p endpoint
   * through @p scheduler, without requiring a connected socket.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_send_to(Scheduler scheduler, Buffer&& buffer,
                                   const async_io::local::endpoint& endpoint,
                                   int flags = 0);

  /**
   * Creates a sender that receives one datagram into @p buffer through
   * @p scheduler and stores the source endpoint into @p endpoint.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_receive_from(Scheduler scheduler, Buffer&& buffer,
                                        async_io::local::endpoint& endpoint,
                                        int flags = 0);

 private:
  native_handle_type fd_ = -1;
};

}  // namespace bnio::local

#endif  // BNIO_LOCAL_SOCKET_H_
