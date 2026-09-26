/**
 * @file socket.h
 * @brief Thin wrappers for the POSIX socket descriptor address queries.
 */

#pragma once
#ifndef BNIO_BASE_SOCKET_H_
#define BNIO_BASE_SOCKET_H_

#include <bnio/base/config.h>

#if defined(BNIO_HAS_BASE_POSIX)

#include <sys/socket.h>

namespace bnio::base {

/**
 * Stores the peer address of a connected socket descriptor into @p address.
 *
 * This is a thin wrapper around getpeername(2): the return value and errno
 * handling match the raw system call.
 *
 * @see getpeername
 */
[[nodiscard]] inline int peer_address(int descriptor, sockaddr* address,
                                      socklen_t* size) noexcept {
  return ::getpeername(descriptor, address, size);
}

/**
 * Stores the locally bound address of a socket descriptor into @p address.
 *
 * This is a thin wrapper around getsockname(2): the return value and errno
 * handling match the raw system call.
 *
 * @see getsockname
 */
[[nodiscard]] inline int local_address(int descriptor, sockaddr* address,
                                       socklen_t* size) noexcept {
  return ::getsockname(descriptor, address, size);
}

}  // namespace bnio::base

#endif  // defined(BNIO_HAS_BASE_POSIX)

#endif  // BNIO_BASE_SOCKET_H_
