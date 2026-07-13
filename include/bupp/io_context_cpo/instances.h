#pragma once
#ifndef BUPP_IO_CONTEXT_CPO_INSTANCES_H_
#define BUPP_IO_CONTEXT_CPO_INSTANCES_H_

#include <bupp/io_context_cpo/connection.h>
#include <bupp/io_context_cpo/poll.h>
#include <bupp/io_context_cpo/read.h>
#include <bupp/io_context_cpo/resolve.h>
#include <bupp/io_context_cpo/write.h>

namespace bupp {

inline constexpr async_read_t async_read{};
inline constexpr async_read_some_t async_read_some{};
inline constexpr async_write_t async_write{};
inline constexpr async_write_some_t async_write_some{};
inline constexpr async_accept_t async_accept{};
inline constexpr async_connect_t async_connect{};
inline constexpr async_poll_t async_poll{};
inline constexpr async_resolve_t async_resolve{};

}  // namespace bupp

#endif  // BUPP_IO_CONTEXT_CPO_INSTANCES_H_
