/**
 * @file acceptor.h
 * @brief RAII local (AF_UNIX) acceptor (listening socket) owner.
 */

#pragma once
#ifndef BNIO_LOCAL_ACCEPTOR_H_
#define BNIO_LOCAL_ACCEPTOR_H_

#include <bnio/async_io/local/protocol.h>
#include <bnio/async_io/local/socket_view.h>
#include <bnio/export.h>
#include <fcntl.h>
#include <sys/socket.h>

#include <system_error>

namespace bnio::local {

/**
 * RAII owner for a native local listening socket descriptor.
 *
 * stream_acceptor closes its descriptor on destruction. It is move-only
 * because duplicating ownership of one descriptor would make close
 * semantics ambiguous. Use view() when an API needs a non-owning async_io
 * local stream socket view. Binding over an existing socket file fails
 * with std::errc::address_in_use; the caller owns ::unlink for stale
 * socket files.
 */
class BNIO_EXPORT stream_acceptor {
 public:
  /**
   * Native listening socket descriptor type.
   */
  using native_handle_type =
      async_io::local::stream_socket_view::native_handle_type;

  /**
   * Creates a closed acceptor owner.
   */
  stream_acceptor() noexcept = default;

  /**
   * Takes ownership of an existing native listening socket descriptor.
   *
   * The underlying descriptor is set to non-blocking mode so that the
   * kqueue backend can skip per-operation fcntl calls.
   */
  explicit stream_acceptor(native_handle_type fd) noexcept : fd_(fd) {
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
  ~stream_acceptor() noexcept;

  /**
   * Copy construction is disabled because the acceptor owns a descriptor.
   */
  stream_acceptor(const stream_acceptor&) = delete;

  /**
   * Copy assignment is disabled because the acceptor owns a descriptor.
   */
  stream_acceptor& operator=(const stream_acceptor&) = delete;

  /**
   * Moves descriptor ownership from another acceptor.
   */
  stream_acceptor(stream_acceptor&& other) noexcept;

  /**
   * Closes the current descriptor and moves descriptor ownership from
   * another acceptor.
   */
  stream_acceptor& operator=(stream_acceptor&& other) noexcept;

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
   * Creates a sender that accepts one local stream connection through the
   * scheduler's queued submission path.
   */
  template <class Scheduler>
  [[nodiscard]] auto async_accept(Scheduler scheduler, int flags = 0);

  /**
   * Opens a local listening socket for a local stream protocol tag.
   */
  [[nodiscard]] std::error_code open(
      async_io::local::stream_protocol protocol) noexcept;

  /**
   * Binds the acceptor to a local endpoint. An endpoint that cannot be
   * converted to sockaddr_un reports std::errc::invalid_argument without
   * a syscall.
   */
  [[nodiscard]] std::error_code bind(
      const async_io::local::endpoint& endpoint) noexcept;

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

 private:
  native_handle_type fd_ = -1;
};

}  // namespace bnio::local

#endif  // BNIO_LOCAL_ACCEPTOR_H_
