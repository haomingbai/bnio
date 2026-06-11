#pragma once
#ifndef BUPP_TCP_H_
#define BUPP_TCP_H_

#include <bupp/async_io/socket_view.h>
#include <bupp/export.h>
#include <bupp/ip.h>
#include <sys/socket.h>

#include <system_error>

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

/**
 * RAII owner for a native TCP listening socket descriptor.
 *
 * tcp_acceptor closes its descriptor on destruction. It is move-only because
 * duplicating ownership of one descriptor would make close semantics ambiguous.
 * Use view() when an API needs a non-owning async_io listening socket view.
 */
class BUPP_EXPORT tcp_acceptor {
 public:
  /**
   * Native listening socket descriptor type.
   */
  using native_handle_type =
      async_io::listening_socket_view::native_handle_type;

  /**
   * Creates a closed acceptor owner.
   */
  tcp_acceptor() noexcept = default;

  /**
   * Takes ownership of an existing native listening socket descriptor.
   */
  explicit tcp_acceptor(native_handle_type fd) noexcept : fd_(fd) {}

  /**
   * Closes the owned descriptor, if any.
   */
  ~tcp_acceptor() noexcept;

  /**
   * Copy construction is disabled because the acceptor owns a descriptor.
   */
  tcp_acceptor(const tcp_acceptor&) = delete;

  /**
   * Copy assignment is disabled because the acceptor owns a descriptor.
   */
  tcp_acceptor& operator=(const tcp_acceptor&) = delete;

  /**
   * Moves descriptor ownership from another acceptor.
   */
  tcp_acceptor(tcp_acceptor&& other) noexcept;

  /**
   * Closes the current descriptor and moves descriptor ownership from another
   * acceptor.
   */
  tcp_acceptor& operator=(tcp_acceptor&& other) noexcept;

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
  [[nodiscard]] async_io::listening_socket_view view() const noexcept {
    return async_io::listening_socket_view(fd_);
  }

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

}  // namespace bupp

#endif  // BUPP_TCP_H_
