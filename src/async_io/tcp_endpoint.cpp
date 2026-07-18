#include <bnio/async_io/ip/tcp.h>
#include <bnio/async_io/ip/udp.h>

namespace bnio::async_io::ip {

endpoint::endpoint() noexcept = default;

endpoint::endpoint(const ip::address& address, std::uint16_t port) noexcept {
  set_address(address);
  set_port(port);
}

endpoint endpoint::loopback_v4(std::uint16_t port) noexcept {
  return endpoint(ip::address::loopback_v4(), port);
}

endpoint endpoint::any_v4(std::uint16_t port) noexcept {
  return endpoint(ip::address::any_v4(), port);
}

endpoint endpoint::loopback_v6(std::uint16_t port) noexcept {
  return endpoint(ip::address::loopback_v6(), port);
}

endpoint endpoint::any_v6(std::uint16_t port) noexcept {
  return endpoint(ip::address::any_v6(), port);
}

void endpoint::reset() noexcept {
  address_.reset();
  port_ = 0;
}

void endpoint::set_address(const ip::address& address) noexcept {
  const auto old_port = port();

  if (address.type() != ip::address::version::unspecified) {
    address_ = address;
    port_ = old_port;
    return;
  }

  reset();
}

void endpoint::set_port(std::uint16_t port) noexcept {
  if (address_.type() == ip::address::version::unspecified) {
    return;
  }
  port_ = port;
}

void endpoint::set_v4_address(std::uint32_t address) noexcept {
  if (version() != ip::address::version::v4) {
    return;
  }
  address_.set_v4(address);
}

void endpoint::set_v4_address(ip::address::v4_bytes address) noexcept {
  if (version() != ip::address::version::v4) {
    return;
  }
  address_.set_v4(address);
}

void endpoint::set_v6_address(ip::address::v6_bytes address) noexcept {
  if (version() != ip::address::version::v6) {
    return;
  }
  address_.set_v6(address);
}

ip::address endpoint::address() const noexcept { return address_; }

ip::address::version endpoint::version() const noexcept {
  return address_.type();
}

std::uint16_t endpoint::port() const noexcept { return port_; }

tcp tcp::v4() noexcept { return tcp(ip::address::version::v4); }

tcp tcp::v6() noexcept { return tcp(ip::address::version::v6); }

ip::address::version tcp::version() const noexcept { return version_; }

tcp::tcp(ip::address::version version) noexcept : version_(version) {}

udp udp::v4() noexcept { return udp(ip::address::version::v4); }

udp udp::v6() noexcept { return udp(ip::address::version::v6); }

ip::address::version udp::version() const noexcept { return version_; }

udp::udp(ip::address::version version) noexcept : version_(version) {}

}  // namespace bnio::async_io::ip
