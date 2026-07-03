#pragma once
#ifndef BUPP_TCP_H_
#define BUPP_TCP_H_

#include <bupp/async_io/socket_view.h>
#include <bupp/buffer.h>
#include <bupp/export.h>
#include <bupp/ip.h>
#include <sys/socket.h>

#include <system_error>
#include <type_traits>
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

  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_read(Scheduler scheduler, Buffer&& buffer,
                                int flags = 0);

  /**
   * Creates a sender for one read operation and submits it immediately instead
   * of placing it in the scheduler's queued I/O batch.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_read_direct(Scheduler scheduler, Buffer&& buffer,
                                       int flags = 0);

  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_write(Scheduler scheduler, Buffer&& buffer,
                                 int flags = 0);

  /**
   * Creates a sender for one write operation and submits it immediately instead
   * of placing it in the scheduler's queued I/O batch.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_write_direct(Scheduler scheduler, Buffer&& buffer,
                                        int flags = 0);

  template <class Scheduler>
  [[nodiscard]] auto async_connect(Scheduler scheduler,
                                   const ip::endpoint& endpoint);

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

  template <class Scheduler>
  [[nodiscard]] auto async_accept(Scheduler scheduler, int flags = 0);

  template <class Scheduler>
  [[nodiscard]] auto async_accept_direct(Scheduler scheduler, int flags = 0);

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

#include <bupp/detail/tcp/async_operations.h>

namespace bupp {

/** @cond BUPP_DETAIL */
namespace detail {

template <class Scheduler, bool DirectSubmit, class Receiver>
void tcp_accept_operation<Scheduler, DirectSubmit, Receiver>::child_receiver::
    set_value(int fd) noexcept {
  bexec::set_value(std::move(operation_->receiver_), tcp_socket(fd));
}

}  // namespace detail
/** @endcond */

template <class Scheduler, class Buffer>
auto tcp_socket::async_read(Scheduler scheduler, Buffer&& buffer, int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_read_sender<scheduler_type, holder_type, false>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto tcp_socket::async_read_direct(Scheduler scheduler, Buffer&& buffer,
                                   int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_read_sender<scheduler_type, holder_type, true>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto tcp_socket::async_write(Scheduler scheduler, Buffer&& buffer, int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_write_sender<scheduler_type, holder_type, false>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto tcp_socket::async_write_direct(Scheduler scheduler, Buffer&& buffer,
                                    int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_write_sender<scheduler_type, holder_type, true>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler>
auto tcp_socket::async_connect(Scheduler scheduler,
                               const ip::endpoint& endpoint) {
  return scheduler.async_connect(view(), endpoint);
}

template <class Scheduler>
auto tcp_socket::async_connect_direct(Scheduler scheduler,
                                      const ip::endpoint& endpoint) {
  return scheduler.async_connect_direct(view(), endpoint);
}

template <class Scheduler>
auto tcp_acceptor::async_accept(Scheduler scheduler, int flags) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::tcp_accept_sender<scheduler_type, false>(std::move(scheduler),
                                                          view(), flags);
}

template <class Scheduler>
auto tcp_acceptor::async_accept_direct(Scheduler scheduler, int flags) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::tcp_accept_sender<scheduler_type, true>(std::move(scheduler),
                                                         view(), flags);
}

template <class Layer>
[[nodiscard]] decltype(auto) get_next_layer(Layer& layer) noexcept {
  return layer.next_layer();
}

template <class Layer>
[[nodiscard]] decltype(auto) get_next_layer(const Layer& layer) noexcept {
  return layer.next_layer();
}

template <class Layer>
[[nodiscard]] decltype(auto) get_lowest_layer(Layer& layer) noexcept {
  return layer.lowest_layer();
}

template <class Layer>
[[nodiscard]] decltype(auto) get_lowest_layer(const Layer& layer) noexcept {
  return layer.lowest_layer();
}

}  // namespace bupp

#endif  // BUPP_TCP_H_
