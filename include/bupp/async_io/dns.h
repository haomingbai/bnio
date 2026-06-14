#pragma once
#ifndef BUPP_ASYNC_IO_DNS_H_
#define BUPP_ASYNC_IO_DNS_H_

#include <bupp/async_io/ip/endpoint.h>
#include <bupp/export.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <system_error>

namespace bupp::async_io {

/**
 * Default maximum host bytes stored by dns_query, excluding the trailing nul.
 */
inline constexpr std::size_t default_dns_host_capacity = 255;

/**
 * Default maximum service bytes stored by dns_query, excluding the trailing
 * nul.
 */
inline constexpr std::size_t default_dns_service_capacity = 32;

/**
 * Transport filter used by DNS address resolution.
 */
enum class dns_transport {
  /**
   * Do not restrict the resolver by socket type or protocol.
   */
  any,

  /**
   * Return stream-oriented TCP endpoints.
   */
  tcp,

  /**
   * Return datagram-oriented UDP endpoints.
   */
  udp,
};

/**
 * Portable resolver query flags.
 */
enum class dns_query_flags : unsigned {
  /**
   * No resolver flags.
   */
  none = 0,

  /**
   * Return wildcard addresses suitable for binding when host is empty.
   */
  passive = 1U << 0U,

  /**
   * Request the canonical name when the platform resolver supports it.
   */
  canonical_name = 1U << 1U,

  /**
   * Treat the host as a numeric address string.
   */
  numeric_host = 1U << 2U,

  /**
   * Treat the service as a numeric port string.
   */
  numeric_service = 1U << 3U,
};

[[nodiscard]] constexpr dns_query_flags operator|(
    dns_query_flags lhs, dns_query_flags rhs) noexcept {
  return static_cast<dns_query_flags>(static_cast<unsigned>(lhs) |
                                      static_cast<unsigned>(rhs));
}

[[nodiscard]] constexpr dns_query_flags operator&(
    dns_query_flags lhs, dns_query_flags rhs) noexcept {
  return static_cast<dns_query_flags>(static_cast<unsigned>(lhs) &
                                      static_cast<unsigned>(rhs));
}

[[nodiscard]] constexpr bool has_dns_query_flag(dns_query_flags flags,
                                                dns_query_flags flag) noexcept {
  return (flags & flag) != dns_query_flags::none;
}

/**
 * Non-owning query view consumed by platform resolver implementations.
 */
struct dns_query_view {
  /**
   * Nul-terminated host name, numeric address, or nullptr for an empty host.
   */
  const char* host = nullptr;

  /**
   * Nul-terminated service name, decimal port, or nullptr for an empty service.
   */
  const char* service = nullptr;

  /**
   * Requested address family.
   */
  ip::address::version address_version = ip::address::version::unspecified;

  /**
   * Requested transport filter.
   */
  dns_transport transport = dns_transport::tcp;

  /**
   * Portable resolver flags.
   */
  dns_query_flags flags = dns_query_flags::none;

  /**
   * Whether the owning query fit inside its fixed buffers.
   */
  bool valid = true;
};

/**
 * Non-owning mutable view over caller-owned endpoint storage.
 *
 * The view does not track how many endpoints are written. Resolver operations
 * report that count through set_value(count) or an output count parameter.
 */
class dns_result_view {
 public:
  using endpoint_type = ip::endpoint;

  /**
   * Creates an empty result view.
   */
  constexpr dns_result_view() noexcept = default;

  /**
   * Creates a result view from writable endpoint storage and capacity.
   */
  constexpr dns_result_view(endpoint_type* data, std::size_t size) noexcept
      : data_(data), size_(size) {}

  /**
   * Creates a result view over a std::array.
   */
  template <std::size_t Size>
  constexpr explicit dns_result_view(
      std::array<endpoint_type, Size>& storage) noexcept
      : dns_result_view(storage.data(), storage.size()) {}

  /**
   * Creates a result view over a C array.
   */
  template <std::size_t Size>
  constexpr explicit dns_result_view(endpoint_type (&storage)[Size]) noexcept
      : dns_result_view(storage, Size) {}

  /**
   * Returns the writable endpoint storage.
   */
  [[nodiscard]] constexpr endpoint_type* data() const noexcept { return data_; }

  /**
   * Returns the maximum number of endpoints that may be written.
   */
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }

  /**
   * Returns whether this view is writable, or intentionally empty.
   */
  [[nodiscard]] constexpr bool valid() const noexcept {
    return data_ != nullptr || size_ == 0;
  }

  /**
   * Returns the endpoint at an index.
   */
  [[nodiscard]] constexpr endpoint_type& operator[](
      std::size_t index) const noexcept {
    return data_[index];
  }

 private:
  endpoint_type* data_ = nullptr;
  std::size_t size_ = 0;
};

/**
 * Fixed-capacity owned DNS query.
 *
 * Host and service strings are copied into inline nul-terminated buffers.
 * If either input exceeds its capacity, valid() becomes false and resolution
 * reports invalid_argument.
 */
template <std::size_t HostCapacity = default_dns_host_capacity,
          std::size_t ServiceCapacity = default_dns_service_capacity>
class basic_dns_query {
 public:
  static_assert(HostCapacity > 0);
  static_assert(ServiceCapacity > 0);

  /**
   * Creates an empty query.
   */
  basic_dns_query() noexcept = default;

  /**
   * Creates a query for a host and service name or decimal port string.
   */
  basic_dns_query(std::string_view host, std::string_view service) noexcept {
    (void)set_host(host);
    (void)set_service(service);
  }

  /**
   * Creates a query for a host and numeric port.
   */
  basic_dns_query(std::string_view host, std::uint16_t port) noexcept {
    (void)set_host(host);
    (void)set_port(port);
  }

  /**
   * Returns the host name or numeric address to resolve.
   */
  [[nodiscard]] std::string_view host() const noexcept {
    return std::string_view(host_.data(), host_size_);
  }

  /**
   * Returns the service name or decimal port string to resolve.
   */
  [[nodiscard]] std::string_view service() const noexcept {
    return std::string_view(service_.data(), service_size_);
  }

  /**
   * Returns the requested address version.
   */
  [[nodiscard]] ip::address::version address_version() const noexcept {
    return address_version_;
  }

  /**
   * Returns the requested transport filter.
   */
  [[nodiscard]] dns_transport transport() const noexcept { return transport_; }

  /**
   * Returns portable resolver flags.
   */
  [[nodiscard]] dns_query_flags flags() const noexcept { return flags_; }

  /**
   * Returns whether all assigned host and service data fit inline.
   */
  [[nodiscard]] bool valid() const noexcept {
    return host_valid_ && service_valid_;
  }

  /**
   * Sets the host name or numeric address to resolve.
   */
  [[nodiscard]] bool set_host(std::string_view host) noexcept {
    return assign(host_, host_size_, host_valid_, host);
  }

  /**
   * Sets the service name or decimal port string to resolve.
   */
  [[nodiscard]] bool set_service(std::string_view service) noexcept {
    return assign(service_, service_size_, service_valid_, service);
  }

  /**
   * Sets the service from a numeric port.
   */
  [[nodiscard]] bool set_port(std::uint16_t port) noexcept {
    service_.fill('\0');
    auto* first = service_.data();
    auto* last = first + ServiceCapacity;
    const auto result = std::to_chars(first, last, port);
    if (result.ec != std::errc()) {
      service_size_ = 0;
      service_valid_ = false;
      return false;
    }

    *result.ptr = '\0';
    service_size_ = static_cast<std::size_t>(result.ptr - first);
    service_valid_ = true;
    return true;
  }

  /**
   * Restricts the query to IPv4, IPv6, or either family.
   */
  void set_address_version(ip::address::version version) noexcept {
    address_version_ = version;
  }

  /**
   * Restricts the query to TCP, UDP, or either transport.
   */
  void set_transport(dns_transport transport) noexcept {
    transport_ = transport;
  }

  /**
   * Sets portable resolver flags.
   */
  void set_flags(dns_query_flags flags) noexcept { flags_ = flags; }

  /**
   * Returns a non-owning view for resolver execution.
   */
  [[nodiscard]] dns_query_view view() const noexcept {
    return dns_query_view{
        host_size_ == 0 ? nullptr : host_.data(),
        service_size_ == 0 ? nullptr : service_.data(),
        address_version_,
        transport_,
        flags_,
        valid(),
    };
  }

 private:
  template <std::size_t Size>
  [[nodiscard]] bool assign(std::array<char, Size>& storage, std::size_t& size,
                            bool& field_valid,
                            std::string_view value) noexcept {
    static_assert(Size > 0);
    constexpr std::size_t capacity = Size - 1;

    storage.fill('\0');
    if (value.size() > capacity) {
      size = 0;
      field_valid = false;
      return false;
    }

    if (!value.empty()) {
      std::memcpy(storage.data(), value.data(), value.size());
    }
    storage[value.size()] = '\0';
    size = value.size();
    field_valid = true;
    return true;
  }

  std::array<char, HostCapacity + 1> host_{};
  std::array<char, ServiceCapacity + 1> service_{};
  std::size_t host_size_ = 0;
  std::size_t service_size_ = 0;
  ip::address::version address_version_ = ip::address::version::unspecified;
  dns_transport transport_ = dns_transport::tcp;
  dns_query_flags flags_ = dns_query_flags::none;
  bool host_valid_ = true;
  bool service_valid_ = true;
};

/**
 * Default DNS query type used by bupp senders.
 */
using dns_query =
    basic_dns_query<default_dns_host_capacity, default_dns_service_capacity>;

/**
 * Error category for platform resolver failures.
 */
[[nodiscard]] BUPP_EXPORT const std::error_category& dns_category() noexcept;

/**
 * Converts a platform resolver result code into a std::error_code.
 */
[[nodiscard]] BUPP_EXPORT std::error_code make_dns_error_code(
    int result) noexcept;

/** @cond BUPP_DETAIL */
namespace detail {

[[nodiscard]] BUPP_EXPORT std::error_code resolve_dns_platform(
    dns_query_view query, dns_result_view result, std::size_t& count) noexcept;

}  // namespace detail
/** @endcond */

/**
 * Resolves a DNS query synchronously into caller-provided endpoint storage.
 */
template <std::size_t HostCapacity, std::size_t ServiceCapacity>
[[nodiscard]] std::error_code resolve_dns(
    const basic_dns_query<HostCapacity, ServiceCapacity>& query,
    dns_result_view result, std::size_t& count) noexcept {
  count = 0;
  if (!result.valid()) {
    return std::make_error_code(std::errc::invalid_argument);
  }

  return detail::resolve_dns_platform(query.view(), result, count);
}

}  // namespace bupp::async_io

#endif  // BUPP_ASYNC_IO_DNS_H_
