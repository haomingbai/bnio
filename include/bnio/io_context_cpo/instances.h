/**
 * @file instances.h
 * @brief I/O CPO instance definitions.
 */

#pragma once
#ifndef BNIO_IO_CONTEXT_CPO_INSTANCES_H_
#define BNIO_IO_CONTEXT_CPO_INSTANCES_H_

#include <bnio/io_context_cpo/connection.h>
#include <bnio/io_context_cpo/poll.h>
#include <bnio/io_context_cpo/read.h>
#include <bnio/io_context_cpo/resolve.h>
#include <bnio/io_context_cpo/write.h>

namespace bnio {

/** CPO instance for the read-all async_read operation. */
inline constexpr async_read_t async_read{};

/** CPO instance for one read attempt (async_read_some). */
inline constexpr async_read_some_t async_read_some{};

/** CPO instance for the write-all async_write operation. */
inline constexpr async_write_t async_write{};

/** CPO instance for one write attempt (async_write_some). */
inline constexpr async_write_some_t async_write_some{};

/** CPO instance for accepting one connection. */
inline constexpr async_accept_t async_accept{};

/** CPO instance for connecting a stream to an endpoint. */
inline constexpr async_connect_t async_connect{};

/** CPO instance for descriptor polling. */
inline constexpr async_poll_t async_poll{};

/** CPO instance for DNS resolution into caller-provided result storage. */
inline constexpr async_resolve_t async_resolve{};

}  // namespace bnio

#endif  // BNIO_IO_CONTEXT_CPO_INSTANCES_H_
