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

/** getpeername(2) wrapper; stores the peer address of a connected socket in
 *  @p address (return value/errno unchanged). */
[[nodiscard]] inline int peer_address(int descriptor, sockaddr* address,
                                      socklen_t* size) noexcept {
  return ::getpeername(descriptor, address, size);
}

/** getsockname(2) wrapper; stores the locally bound address of a socket in
 *  @p address (return value/errno unchanged). */
[[nodiscard]] inline int local_address(int descriptor, sockaddr* address,
                                       socklen_t* size) noexcept {
  return ::getsockname(descriptor, address, size);
}

}  // namespace bnio::base

#endif  // defined(BNIO_HAS_BASE_POSIX)

#endif  // BNIO_BASE_SOCKET_H_
