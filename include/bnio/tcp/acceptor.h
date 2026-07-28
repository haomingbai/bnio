/**
 * @file acceptor.h
 * @brief RAII TCP acceptor (listening socket) owner.
 */

#pragma once
#ifndef BNIO_TCP_ACCEPTOR_H_
#define BNIO_TCP_ACCEPTOR_H_

#include <bnio/async_io/socket_view.h>
#include <bnio/export.h>
#include <bnio/ip.h>
#include <fcntl.h>
#include <sys/socket.h>

#include <system_error>

namespace bnio::tcp {

/**
 * RAII owner for a native TCP listening socket descriptor.
 *
 * tcp_acceptor closes its descriptor on destruction. It is move-only because
 * duplicating ownership of one descriptor would make close semantics ambiguous.
 * Use view() when an API needs a non-owning async_io stream socket view.
 */
class BNIO_EXPORT acceptor {
 public:
  /**
   * Native listening socket descriptor type.
   */
  using native_handle_type = async_io::stream_socket_view::native_handle_type;

  /**
   * Creates a closed acceptor owner.
   */
  acceptor() noexcept = default;

  /**
   * Takes ownership of an existing native listening socket descriptor.
   *
   * The underlying descriptor is set to non-blocking mode so that the
   * kqueue backend can skip per-operation fcntl calls.
   */
  explicit acceptor(native_handle_type fd) noexcept : fd_(fd) {
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
  ~acceptor() noexcept;

  /**
   * Copy construction is disabled because the acceptor owns a descriptor.
   */
  acceptor(const acceptor&) = delete;

  /**
   * Copy assignment is disabled because the acceptor owns a descriptor.
   */
  acceptor& operator=(const acceptor&) = delete;

  /**
   * Moves descriptor ownership from another acceptor.
   */
  acceptor(acceptor&& other) noexcept;

  /**
   * Closes the current descriptor and moves descriptor ownership from another
   * acceptor.
   */
  acceptor& operator=(acceptor&& other) noexcept;

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
   * Creates a sender that accepts one TCP connection through the scheduler's
   * queued submission path.
   */
  template <class Scheduler>
  [[nodiscard]] auto async_accept(Scheduler scheduler, int flags = 0);

  /**
   * Opens a TCP listening socket for an address family.
   */
  [[nodiscard]] std::error_code open(int family = AF_INET) noexcept;

  /**
   * Opens a TCP listening socket for a TCP protocol tag.
   */
  [[nodiscard]] std::error_code open(ip::tcp protocol) noexcept;

  /**
   * Binds the acceptor to an endpoint.
   */
  [[nodiscard]] std::error_code bind(const ip::endpoint& endpoint) noexcept;

  /**
   * Marks the acceptor as a listening socket.
   */
  [[nodiscard]] std::error_code listen(int backlog) noexcept;

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

#endif  // BNIO_TCP_ACCEPTOR_H_
