#pragma once
#ifndef BNIO_ASYNC_IO_IP_ADDRESS_H_
#define BNIO_ASYNC_IO_IP_ADDRESS_H_

#include <bnio/async_io/config.h>
#include <bnio/export.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace bnio::async_io::ip {

/**
 * Value type representing an IPv4, IPv6, or unspecified IP address.
 *
 * Addresses own their bytes directly and are cheap to copy or move.
 */
class BNIO_EXPORT address {
 public:
  /**
   * IPv4 address bytes in network byte order.
   */
  using v4_bytes = std::array<std::uint8_t, 4>;

  /**
   * IPv6 address bytes in network byte order.
   */
  using v6_bytes = std::array<std::uint8_t, 16>;

  /**
   * Address family stored by an address.
   */
  enum class version {
    /**
     * No address family is set.
     */
    unspecified,

    /**
     * IPv4 address family.
     */
    v4,

    /**
     * IPv6 address family.
     */
    v6,
  };

  /**
   * Creates an unspecified address.
   */
  address() noexcept;

  /**
   * Copies an address value.
   */
  address(const address&) noexcept = default;

  /**
   * Copies an address value.
   */
  address& operator=(const address&) noexcept = default;

  /**
   * Moves an address value.
   */
  address(address&&) noexcept = default;

  /**
   * Moves an address value.
   */
  address& operator=(address&&) noexcept = default;

  /**
   * Destroys the address value.
   */
  ~address() noexcept = default;

  /**
   * Returns the IPv4 wildcard address.
   */
  static address any_v4() noexcept;

  /**
   * Returns the IPv4 loopback address.
   */
  static address loopback_v4() noexcept;

  /**
   * Creates an IPv4 address from a host-order integer.
   */
  static address v4(std::uint32_t address) noexcept;

  /**
   * Creates an IPv4 address from network-order bytes.
   */
  static address v4(v4_bytes address) noexcept;

  /**
   * Returns the IPv6 wildcard address.
   */
  static address any_v6() noexcept;

  /**
   * Returns the IPv6 loopback address.
   */
  static address loopback_v6() noexcept;

  /**
   * Creates an IPv6 address from network-order bytes.
   */
  static address v6(v6_bytes address) noexcept;

  /**
   * Resets the address to the unspecified state.
   */
  void reset() noexcept;

  /**
   * Stores an IPv4 address from a host-order integer.
   */
  void set_v4(std::uint32_t address) noexcept;

  /**
   * Stores an IPv4 address from network-order bytes.
   */
  void set_v4(v4_bytes address) noexcept;

  /**
   * Stores an IPv6 address from network-order bytes.
   */
  void set_v6(v6_bytes address) noexcept;

  /**
   * Returns the stored address family.
   */
  [[nodiscard]] version type() const noexcept;

  /**
   * Returns whether this address stores an IPv4 value.
   */
  [[nodiscard]] bool is_v4() const noexcept;

  /**
   * Returns whether this address stores an IPv6 value.
   */
  [[nodiscard]] bool is_v6() const noexcept;

  /**
   * Returns the IPv4 bytes, or nullptr when this is not an IPv4 address.
   */
  [[nodiscard]] const v4_bytes* v4() const noexcept;

  /**
   * Returns the IPv6 bytes, or nullptr when this is not an IPv6 address.
   */
  [[nodiscard]] const v6_bytes* v6() const noexcept;

  /**
   * Returns the IPv4 address as a host-order integer, or zero otherwise.
   */
  [[nodiscard]] std::uint32_t to_v4() const noexcept;

 private:
  version type_ = version::unspecified;
  v4_bytes v4_{};
  v6_bytes v6_{};
};

#if defined(BNIO_HAS_ASYNC_IO_IP_ADDRESS_PARSER)
/**
 * Parses an IPv4 or IPv6 address string.
 */
[[nodiscard]] BNIO_EXPORT std::optional<address> make_address(
    std::string_view text);

/**
 * Parses an IPv4 or IPv6 address string.
 */
[[nodiscard]] BNIO_EXPORT std::optional<address> make_addr(
    std::string_view text);

/**
 * Parses an IPv4 address string.
 */
[[nodiscard]] BNIO_EXPORT std::optional<address> make_v4_address(
    std::string_view text);

/**
 * Parses an IPv6 address string.
 */
[[nodiscard]] BNIO_EXPORT std::optional<address> make_v6_address(
    std::string_view text);
#endif

}  // namespace bnio::async_io::ip

#endif  // BNIO_ASYNC_IO_IP_ADDRESS_H_
