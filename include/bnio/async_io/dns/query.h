#pragma once
#ifndef BNIO_ASYNC_IO_DNS_QUERY_H_
#define BNIO_ASYNC_IO_DNS_QUERY_H_

#include <bnio/async_io/dns/types.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <system_error>

namespace bnio::async_io {

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
 * Default DNS query type used by bnio senders.
 */
using dns_query =
    basic_dns_query<default_dns_host_capacity, default_dns_service_capacity>;

}  // namespace bnio::async_io

#endif  // BNIO_ASYNC_IO_DNS_QUERY_H_
