#pragma once
#ifndef BNIO_ASYNC_IO_IP_UDP_H_
#define BNIO_ASYNC_IO_IP_UDP_H_

#include <bnio/async_io/ip/endpoint.h>
#include <bnio/export.h>

namespace bnio::async_io::ip {

/**
 * Protocol tag for UDP over IPv4 or IPv6.
 *
 * Protocol tags are small value types and are cheap to copy or move.
 */
class BNIO_EXPORT udp {
 public:
  /**
   * Endpoint type used by UDP.
   */
  using endpoint = ip::endpoint;

  /**
   * Creates an unspecified UDP protocol tag.
   */
  udp() noexcept = default;

  /**
   * Copies a UDP protocol tag.
   */
  udp(const udp&) noexcept = default;

  /**
   * Copies a UDP protocol tag.
   */
  udp& operator=(const udp&) noexcept = default;

  /**
   * Moves a UDP protocol tag.
   */
  udp(udp&&) noexcept = default;

  /**
   * Moves a UDP protocol tag.
   */
  udp& operator=(udp&&) noexcept = default;

  /**
   * Destroys the UDP protocol tag.
   */
  ~udp() noexcept = default;

  /**
   * Returns a UDP/IPv4 protocol tag.
   */
  static udp v4() noexcept;

  /**
   * Returns a UDP/IPv6 protocol tag.
   */
  static udp v6() noexcept;

  /**
   * Returns the IP version associated with this protocol tag.
   */
  [[nodiscard]] ip::address::version version() const noexcept;

 private:
  /**
   * Creates a UDP protocol tag for an IP version.
   */
  explicit udp(ip::address::version version) noexcept;

  ip::address::version version_ = ip::address::version::unspecified;
};

}  // namespace bnio::async_io::ip

#endif  // BNIO_ASYNC_IO_IP_UDP_H_
