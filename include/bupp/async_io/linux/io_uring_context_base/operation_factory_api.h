#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CLASS_SCOPE_
#error "This header is an io_uring_context class declaration fragment."
#endif

#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_OPERATION_FACTORY_API_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_OPERATION_FACTORY_API_H_

/**
 * Creates a sender that waits for events on a file descriptor.
 */
[[nodiscard]] auto async_poll(bupp::async_io::descriptor_view descriptor,
                              unsigned poll_mask);

/**
 * Creates a sender that resolves a DNS query into caller-provided result
 * storage on the context run loop.
 */
[[nodiscard]] auto async_resolve(bupp::async_io::dns_query query,
                                 bupp::async_io::dns_result_view result);

/**
 * Creates a sender that resolves a host and service into caller-provided
 * result storage on the context run loop.
 */
[[nodiscard]] auto async_resolve(std::string_view host,
                                 std::string_view service,
                                 bupp::async_io::dns_result_view result);

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_OPERATION_FACTORY_API_H_
