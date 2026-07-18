#pragma once
#ifndef BNIO_ASYNC_IO_IP_ENDPOINT_H_
#define BNIO_ASYNC_IO_IP_ENDPOINT_H_

#include <bnio/async_io/ip/address.h>
#include <bnio/export.h>

#include <cstdint>

namespace bnio::async_io::ip {

/**
 * Value type representing an IP address and port pair.
 *
 * Endpoints own their address and port directly and are cheap to copy or move.
 */
class BNIO_EXPORT endpoint {
 public:
  /**
   * Creates an unspecified endpoint.
   */
  endpoint() noexcept;

  /**
   * Creates an endpoint from an IP address and port.
   */
  endpoint(const ip::address& address, std::uint16_t port) noexcept;

  /**
   * Copies an endpoint value.
   */
  endpoint(const endpoint&) noexcept = default;

  /**
   * Copies an endpoint value.
   */
  endpoint& operator=(const endpoint&) noexcept = default;

  /**
   * Moves an endpoint value.
   */
  endpoint(endpoint&&) noexcept = default;

  /**
   * Moves an endpoint value.
   */
  endpoint& operator=(endpoint&&) noexcept = default;

  /**
   * Destroys the endpoint value.
   */
  ~endpoint() noexcept = default;

  /**
   * Returns an IPv4 loopback endpoint for a port.
   */
  static endpoint loopback_v4(std::uint16_t port) noexcept;

  /**
   * Returns an IPv4 wildcard endpoint for a port.
   */
  static endpoint any_v4(std::uint16_t port) noexcept;

  /**
   * Returns an IPv6 loopback endpoint for a port.
   */
  static endpoint loopback_v6(std::uint16_t port) noexcept;

  /**
   * Returns an IPv6 wildcard endpoint for a port.
   */
  static endpoint any_v6(std::uint16_t port) noexcept;

  /**
   * Resets the endpoint to an unspecified address and port zero.
   */
  void reset() noexcept;

  /**
   * Sets the endpoint address and preserves the existing port when possible.
   */
  void set_address(const ip::address& address) noexcept;

  /**
   * Sets the endpoint port when the endpoint has an address family.
   */
  void set_port(std::uint16_t port) noexcept;

  /**
   * Replaces the IPv4 address from a host-order integer.
   */
  void set_v4_address(std::uint32_t address) noexcept;

  /**
   * Replaces the IPv4 address from network-order bytes.
   */
  void set_v4_address(ip::address::v4_bytes address) noexcept;

  /**
   * Replaces the IPv6 address from network-order bytes.
   */
  void set_v6_address(ip::address::v6_bytes address) noexcept;

  /**
   * Returns the endpoint address.
   */
  [[nodiscard]] ip::address address() const noexcept;

  /**
   * Returns the endpoint address family.
   */
  [[nodiscard]] ip::address::version version() const noexcept;

  /**
   * Returns the endpoint port.
   */
  [[nodiscard]] std::uint16_t port() const noexcept;

 private:
  ip::address address_;
  std::uint16_t port_ = 0;
};

}  // namespace bnio::async_io::ip

#endif  // BNIO_ASYNC_IO_IP_ENDPOINT_H_
