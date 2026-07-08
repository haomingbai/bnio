#pragma once
#ifndef BUPP_IP_H_
#define BUPP_IP_H_

#include <bupp/async_io/dns.h>
#include <bupp/async_io/ip/address.h>
#include <bupp/async_io/ip/endpoint.h>
#include <bupp/async_io/ip/tcp.h>
#include <bupp/async_io/ip/udp.h>
#include <bupp/async_io/config.h>
#include <bupp/export.h>

namespace bupp {

class BUPP_EXPORT tcp_socket;
class BUPP_EXPORT tcp_acceptor;

/**
 * DNS query object used by io_context resolver senders.
 */
using dns_query = async_io::dns_query;

/**
 * DNS result storage view used by io_context resolver senders.
 */
using dns_result_view = async_io::dns_result_view;

/**
 * DNS transport filter used by resolver queries.
 */
using dns_transport = async_io::dns_transport;

/**
 * DNS resolver query flags.
 */
using dns_query_flags = async_io::dns_query_flags;

namespace ip {

/**
 * Alias for the generic IP address type.
 */
using address = async_io::ip::address;

/**
 * Alias for a generic IP endpoint.
 */
using endpoint = async_io::ip::endpoint;

#if defined(BUPP_HAS_ASYNC_IO_IP_ADDRESS_PARSER)
/**
 * Imports the generic IP address parser into bupp::ip.
 */
using async_io::ip::make_addr;

/**
 * Imports the generic IP address parser into bupp::ip.
 */
using async_io::ip::make_address;

/**
 * Imports the IPv4 address parser into bupp::ip.
 */
using async_io::ip::make_v4_address;

/**
 * Imports the IPv6 address parser into bupp::ip.
 */
using async_io::ip::make_v6_address;
#endif

/**
 * Protocol tag and type namespace for TCP over IPv4 or IPv6.
 *
 * The address and endpoint vocabulary types are aliases of async_io value
 * types, while socket owners are provided by the higher-level TCP API.
 */
class BUPP_EXPORT tcp {
 public:
  /**
   * Endpoint type used by TCP.
   */
  using endpoint = bupp::ip::endpoint;

  /**
   * Owning TCP stream socket type.
   */
  using socket = bupp::tcp_socket;

  /**
   * Owning TCP stream socket type.
   */
  using stream = socket;

  /**
   * Owning TCP listening socket type.
   */
  using acceptor = bupp::tcp_acceptor;

  /**
   * Creates an unspecified TCP protocol tag.
   */
  tcp() noexcept = default;

  /**
   * Copies a TCP protocol tag.
   */
  tcp(const tcp&) noexcept = default;

  /**
   * Copies a TCP protocol tag.
   */
  tcp& operator=(const tcp&) noexcept = default;

  /**
   * Moves a TCP protocol tag.
   */
  tcp(tcp&&) noexcept = default;

  /**
   * Moves a TCP protocol tag.
   */
  tcp& operator=(tcp&&) noexcept = default;

  /**
   * Destroys the TCP protocol tag.
   */
  ~tcp() noexcept = default;

  /**
   * Returns a TCP/IPv4 protocol tag.
   */
  static tcp v4() noexcept { return tcp(async_io::ip::tcp::v4()); }

  /**
   * Returns a TCP/IPv6 protocol tag.
   */
  static tcp v6() noexcept { return tcp(async_io::ip::tcp::v6()); }

  /**
   * Returns the IP version associated with this protocol tag.
   */
  [[nodiscard]] bupp::ip::address::version version() const noexcept {
    return protocol_.version();
  }

  /**
   * Returns the async_io protocol tag represented by this value.
   */
  [[nodiscard]] async_io::ip::tcp async_io_protocol() const noexcept {
    return protocol_;
  }

 private:
  /**
   * Creates a TCP protocol tag from an async_io protocol tag.
   */
  explicit tcp(async_io::ip::tcp protocol) noexcept : protocol_(protocol) {}

  async_io::ip::tcp protocol_{};
};

/**
 * Alias for the UDP protocol tag.
 */
using udp = async_io::ip::udp;

}  // namespace ip

}  // namespace bupp

#endif  // BUPP_IP_H_
