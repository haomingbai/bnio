#pragma once
#ifndef BUPP_LINUX_IO_CONTEXT_CLASS_SCOPE_
#error "This header is an io_context class declaration fragment."
#endif

#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_PRIVATE_IO_MEMBERS_H_
#define BUPP_LINUX_DETAIL_IO_CONTEXT_PRIVATE_IO_MEMBERS_H_

private:
friend class steady_timer;
friend class detail::timer_operation_base;
template <class Receiver>
friend class detail::timer_wait_operation;
template <class Model, class Receiver>
friend class detail::native_io_operation;

/**
 * Submits all operations currently waiting in the queued I/O list.
 */
[[nodiscard]] std::error_code flush_io_queue() noexcept;

/**
 * Returns the number of operations waiting in the queued I/O list.
 */
[[nodiscard]] std::size_t queued_io_size() const noexcept;

/**
 * Returns the underlying Linux native async I/O context.
 */
[[nodiscard]] async_io::linux_native::io_uring_context&
native_context() noexcept {
  return native_context_;
}

/**
 * Returns the underlying Linux native async I/O context.
 */
[[nodiscard]] const async_io::linux_native::io_uring_context& native_context()
    const noexcept {
  return native_context_;
}

/**
 * Creates a queued sender that reads bytes from a non-owning stream socket
 * view and completes with bytes transferred.
 */
[[nodiscard]] auto async_read(async_io::stream_socket_view socket,
                              mutable_buffer buffer, int flags = 0);

[[nodiscard]] auto async_read_some(async_io::stream_socket_view socket,
                                   mutable_buffer buffer, int flags = 0);

/**
 * Creates a direct-submission sender that reads bytes from a non-owning
 * stream socket view and completes with bytes transferred.
 */
[[nodiscard]] auto async_read_direct(async_io::stream_socket_view socket,
                                     mutable_buffer buffer, int flags = 0);

[[nodiscard]] auto async_read_some_direct(async_io::stream_socket_view socket,
                                          mutable_buffer buffer, int flags = 0);

/**
 * Creates a queued sender that writes the whole buffer through a non-owning
 * stream socket view.
 */
[[nodiscard]] auto async_write(async_io::stream_socket_view socket,
                               const_buffer buffer, int flags = 0);

/**
 * Creates a queued sender for one write operation through a non-owning stream
 * socket view.
 */
[[nodiscard]] auto async_write_some(async_io::stream_socket_view socket,
                                    const_buffer buffer, int flags = 0);

/**
 * Creates a direct-submission sender that writes the whole buffer through a
 * non-owning stream socket view.
 */
[[nodiscard]] auto async_write_direct(async_io::stream_socket_view socket,
                                      const_buffer buffer, int flags = 0);

/**
 * Creates a direct-submission sender for one write operation through a
 * non-owning stream socket view.
 */
[[nodiscard]] auto async_write_some_direct(async_io::stream_socket_view socket,
                                           const_buffer buffer, int flags = 0);

/**
 * Creates a queued sender that reads bytes from a file descriptor.
 */
[[nodiscard]] auto async_read(async_io::descriptor_view descriptor,
                              mutable_buffer buffer, std::uint64_t offset = 0);

[[nodiscard]] auto async_read_some(async_io::descriptor_view descriptor,
                                   mutable_buffer buffer,
                                   std::uint64_t offset = 0);

/**
 * Creates a direct-submission sender that reads bytes from a file descriptor.
 */
[[nodiscard]] auto async_read_direct(async_io::descriptor_view descriptor,
                                     mutable_buffer buffer,
                                     std::uint64_t offset = 0);

[[nodiscard]] auto async_read_some_direct(async_io::descriptor_view descriptor,
                                          mutable_buffer buffer,
                                          std::uint64_t offset = 0);

/**
 * Creates a queued sender that writes the whole buffer to a file descriptor.
 */
[[nodiscard]] auto async_write(async_io::descriptor_view descriptor,
                               const_buffer buffer, std::uint64_t offset = 0);

/**
 * Creates a queued sender for one write operation to a file descriptor.
 */
[[nodiscard]] auto async_write_some(async_io::descriptor_view descriptor,
                                    const_buffer buffer,
                                    std::uint64_t offset = 0);

/**
 * Creates a direct-submission sender that writes the whole buffer to a file
 * descriptor.
 */
[[nodiscard]] auto async_write_direct(async_io::descriptor_view descriptor,
                                      const_buffer buffer,
                                      std::uint64_t offset = 0);

/**
 * Creates a direct-submission sender for one write operation to a file
 * descriptor.
 */
[[nodiscard]] auto async_write_some_direct(async_io::descriptor_view descriptor,
                                           const_buffer buffer,
                                           std::uint64_t offset = 0);

/**
 * Creates a queued sender that accepts one connection from a non-owning
 * listening socket view.
 */
[[nodiscard]] auto async_accept(async_io::listening_socket_view socket,
                                int flags = 0);

/**
 * Creates a direct-submission sender that accepts one connection from a
 * non-owning listening socket view.
 */
[[nodiscard]] auto async_accept_direct(async_io::listening_socket_view socket,
                                       int flags = 0);

/**
 * Creates a queued sender that connects a non-owning stream socket view.
 */
[[nodiscard]] auto async_connect(async_io::stream_socket_view socket,
                                 const ip::endpoint& endpoint);

/**
 * Creates a direct-submission sender that connects a non-owning stream socket
 * view.
 */
[[nodiscard]] auto async_connect_direct(async_io::stream_socket_view socket,
                                        const ip::endpoint& endpoint);

/**
 * Creates a queued sender that waits for events on a file descriptor.
 */
[[nodiscard]] auto async_poll(async_io::descriptor_view descriptor,
                              unsigned poll_mask);

/**
 * Creates a direct-submission sender that waits for descriptor events.
 */
[[nodiscard]] auto async_poll_direct(async_io::descriptor_view descriptor,
                                     unsigned poll_mask);

/**
 * Creates a sender that resolves a DNS query into caller-provided result
 * storage on the context run loop.
 */
[[nodiscard]] auto async_resolve(async_io::dns_query query,
                                 async_io::dns_result_view result);

/**
 * Creates a sender that resolves a host and service into caller-provided
 * result storage on the context run loop.
 */
[[nodiscard]] auto async_resolve(std::string_view host,
                                 std::string_view service,
                                 async_io::dns_result_view result);

/**
 * Queues an operation for batched io_uring submission.
 */
void enqueue_io(operation_base& operation) noexcept;

/**
 * Submits an operation immediately through the native context.
 */
void submit_direct(operation_base& operation) noexcept;

/**
 * Posts an operation for execution on the context run loop.
 */
void post(async_io::linux_native::io_uring_operation_base& operation) noexcept;

#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_PRIVATE_IO_MEMBERS_H_
