#pragma once
#ifndef BUPP_ASYNC_IO_IP_TCP_H_
#define BUPP_ASYNC_IO_IP_TCP_H_

#include <bupp/async_io/ip/endpoint.h>
#include <bupp/export.h>

namespace bupp::async_io::ip {

/**
 * Protocol tag for TCP over IPv4 or IPv6.
 *
 * Protocol tags are small value types and are cheap to copy or move.
 */
class BUPP_EXPORT tcp {
 public:
  /**
   * Endpoint type used by TCP.
   */
  using endpoint = ip::endpoint;

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
  static tcp v4() noexcept;

  /**
   * Returns a TCP/IPv6 protocol tag.
   */
  static tcp v6() noexcept;

  /**
   * Returns the IP version associated with this protocol tag.
   */
  [[nodiscard]] ip::address::version version() const noexcept;

 private:
  /**
   * Creates a TCP protocol tag for an IP version.
   */
  explicit tcp(ip::address::version version) noexcept;

  ip::address::version version_ = ip::address::version::unspecified;
};

}  // namespace bupp::async_io::ip

#endif  // BUPP_ASYNC_IO_IP_TCP_H_
