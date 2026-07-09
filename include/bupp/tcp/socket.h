#pragma once
#ifndef BUPP_TCP_SOCKET_H_
#define BUPP_TCP_SOCKET_H_

#include <bupp/async_io/socket_view.h>
#include <bupp/buffer.h>
#include <bupp/export.h>
#include <bupp/ip.h>
#include <sys/socket.h>

#include <system_error>
#include <utility>

namespace bupp {

/**
 * RAII owner for a native TCP stream socket descriptor.
 *
 * tcp_socket closes its descriptor on destruction. It is move-only because
 * duplicating ownership of one descriptor would make close semantics ambiguous.
 * Use view() when an API needs a non-owning async_io stream socket view.
 */
class BUPP_EXPORT tcp_socket {
 public:
  /**
   * Native stream socket descriptor type.
   */
  using native_handle_type = async_io::stream_socket_view::native_handle_type;

  /**
   * Creates a closed socket owner.
   */
  tcp_socket() noexcept = default;

  /**
   * Takes ownership of an existing native stream socket descriptor.
   */
  explicit tcp_socket(native_handle_type fd) noexcept : fd_(fd) {}

  /**
   * Closes the owned descriptor, if any.
   */
  ~tcp_socket() noexcept;

  /**
   * Copy construction is disabled because the socket owns a descriptor.
   */
  tcp_socket(const tcp_socket&) = delete;

  /**
   * Copy assignment is disabled because the socket owns a descriptor.
   */
  tcp_socket& operator=(const tcp_socket&) = delete;

  /**
   * Moves descriptor ownership from another socket.
   */
  tcp_socket(tcp_socket&& other) noexcept;

  /**
   * Closes the current descriptor and moves descriptor ownership from another
   * socket.
   */
  tcp_socket& operator=(tcp_socket&& other) noexcept;

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
  [[nodiscard]] tcp_socket& next_layer() noexcept { return *this; }

  /**
   * Returns this stream's next layer.
   */
  [[nodiscard]] const tcp_socket& next_layer() const noexcept { return *this; }

  /**
   * Returns this stream's lowest layer.
   */
  [[nodiscard]] tcp_socket& lowest_layer() noexcept { return *this; }

  /**
   * Returns this stream's lowest layer.
   */
  [[nodiscard]] const tcp_socket& lowest_layer() const noexcept {
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
   * Creates a sender for one read operation and submits it immediately instead
   * of placing it in the scheduler's queued I/O batch.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_read_direct(Scheduler scheduler, Buffer&& buffer,
                                       int flags = 0);

  /**
   * Creates a sender for one direct-submission read operation.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_read_some_direct(Scheduler scheduler,
                                            Buffer&& buffer, int flags = 0);

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
   * Creates a sender that writes the whole buffer and submits each lower-level
   * write immediately instead of placing it in the scheduler's queued I/O
   * batch.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_write_direct(Scheduler scheduler, Buffer&& buffer,
                                        int flags = 0);

  /**
   * Creates a sender for one direct-submission write operation without retrying
   * short writes.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_write_some_direct(Scheduler scheduler,
                                             Buffer&& buffer, int flags = 0);

  /**
   * Creates a sender that connects this socket to an endpoint through the
   * scheduler's queued submission path.
   */
  template <class Scheduler>
  [[nodiscard]] auto async_connect(Scheduler scheduler,
                                   const ip::endpoint& endpoint);

  /**
   * Creates a sender that connects this socket to an endpoint through direct
   * submission.
   */
  template <class Scheduler>
  [[nodiscard]] auto async_connect_direct(Scheduler scheduler,
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

}  // namespace bupp

#endif  // BUPP_TCP_SOCKET_H_
