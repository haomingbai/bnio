/**
 * @file protocol.h
 * @brief Local (AF_UNIX) protocol tags.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LOCAL_PROTOCOL_H_
#define BNIO_ASYNC_IO_LOCAL_PROTOCOL_H_

#include <bnio/async_io/local/endpoint.h>
#include <sys/socket.h>

namespace bnio::local {
// Owner types provided by the higher-level local API (bnio/local.h); named
// here only so the protocol tags can carry Asio-style discovery typedefs.
class stream_socket;
class stream_acceptor;
class datagram_socket;
}  // namespace bnio::local

namespace bnio::async_io::local {

/**
 * Protocol tag for a local (AF_UNIX) byte-stream socket.
 *
 * Protocol tags are stateless value types in the role of ip::tcp /
 * ip::udp: they carry the socket(2) triple consumed at open().
 */
class stream_protocol {
 public:
  /**
   * Endpoint type used by local stream sockets.
   */
  using endpoint = local::endpoint;

  /**
   * Owning local stream socket type.
   */
  using socket = bnio::local::stream_socket;

  /**
   * Owning local listening socket type.
   */
  using acceptor = bnio::local::stream_acceptor;

  /**
   * Returns the socket type: SOCK_STREAM.
   */
  [[nodiscard]] int type() const noexcept { return SOCK_STREAM; }

  /**
   * Returns the socket protocol: 0 (no protocol selection within AF_UNIX).
   */
  [[nodiscard]] int protocol() const noexcept { return 0; }

  /**
   * Returns the address family: AF_UNIX.
   */
  [[nodiscard]] int family() const noexcept { return AF_UNIX; }
};

/**
 * Protocol tag for a local (AF_UNIX) datagram socket, preserving message
 * boundaries.
 */
class datagram_protocol {
 public:
  /**
   * Endpoint type used by local datagram sockets.
   */
  using endpoint = local::endpoint;

  /**
   * Owning local datagram socket type.
   */
  using socket = bnio::local::datagram_socket;

  /**
   * Returns the socket type: SOCK_DGRAM.
   */
  [[nodiscard]] int type() const noexcept { return SOCK_DGRAM; }

  /**
   * Returns the socket protocol: 0 (no protocol selection within AF_UNIX).
   */
  [[nodiscard]] int protocol() const noexcept { return 0; }

  /**
   * Returns the address family: AF_UNIX.
   */
  [[nodiscard]] int family() const noexcept { return AF_UNIX; }
};

}  // namespace bnio::async_io::local

#endif  // BNIO_ASYNC_IO_LOCAL_PROTOCOL_H_
