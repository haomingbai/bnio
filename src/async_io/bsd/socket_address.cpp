#include <arpa/inet.h>
#include <bupp/async_io/bsd/socket_address.h>

#include <cstring>

namespace bupp::async_io::bsd_native {
namespace {

void copy_v4_address(sockaddr_in& output,
                     const ip::address::v4_bytes& input) noexcept {
  std::memcpy(&output.sin_addr, input.data(), input.size());
}

void copy_v6_address(sockaddr_in6& output,
                     const ip::address::v6_bytes& input) noexcept {
  std::memcpy(&output.sin6_addr, input.data(), input.size());
}

ip::address::v4_bytes load_v4_address(const sockaddr_in& input) noexcept {
  ip::address::v4_bytes output{};
  std::memcpy(output.data(), &input.sin_addr, output.size());
  return output;
}

ip::address::v6_bytes load_v6_address(const sockaddr_in6& input) noexcept {
  ip::address::v6_bytes output{};
  std::memcpy(output.data(), &input.sin6_addr, output.size());
  return output;
}

}  // namespace

socket_address::socket_address() noexcept = default;

socket_address::socket_address(const ip::endpoint& endpoint) noexcept {
  const auto address = endpoint.address();

  if (const auto* bytes = address.v4()) {
    auto* native = reinterpret_cast<sockaddr_in*>(&storage_);
    *native = {};
    native->sin_family = static_cast<sa_family_t>(AF_INET);
    native->sin_port = htons(endpoint.port());
    copy_v4_address(*native, *bytes);
    size_ = static_cast<socklen_t>(sizeof(sockaddr_in));
    return;
  }

  if (const auto* bytes = address.v6()) {
    auto* native = reinterpret_cast<sockaddr_in6*>(&storage_);
    *native = {};
    native->sin6_family = static_cast<sa_family_t>(AF_INET6);
    native->sin6_port = htons(endpoint.port());
    copy_v6_address(*native, *bytes);
    size_ = static_cast<socklen_t>(sizeof(sockaddr_in6));
  }
}

bool socket_address::valid() const noexcept { return size_ != 0; }

int socket_address::family() const noexcept {
  return valid() ? static_cast<int>(storage_.ss_family) : AF_UNSPEC;
}

sockaddr* socket_address::data() noexcept {
  return valid() ? reinterpret_cast<sockaddr*>(&storage_) : nullptr;
}

const sockaddr* socket_address::data() const noexcept {
  return valid() ? reinterpret_cast<const sockaddr*>(&storage_) : nullptr;
}

socklen_t socket_address::size() const noexcept { return size_; }

std::optional<ip::endpoint> make_endpoint(const sockaddr* address,
                                          socklen_t size) noexcept {
  if (address == nullptr ||
      size < static_cast<socklen_t>(sizeof(sa_family_t))) {
    return std::nullopt;
  }

  switch (address->sa_family) {
    case AF_INET: {
      if (size < static_cast<socklen_t>(sizeof(sockaddr_in))) {
        return std::nullopt;
      }
      const auto* native = reinterpret_cast<const sockaddr_in*>(address);
      return ip::endpoint(ip::address::v4(load_v4_address(*native)),
                          static_cast<std::uint16_t>(ntohs(native->sin_port)));
    }
    case AF_INET6: {
      if (size < static_cast<socklen_t>(sizeof(sockaddr_in6))) {
        return std::nullopt;
      }
      const auto* native = reinterpret_cast<const sockaddr_in6*>(address);
      return ip::endpoint(ip::address::v6(load_v6_address(*native)),
                          static_cast<std::uint16_t>(ntohs(native->sin6_port)));
    }
    default:
      return std::nullopt;
  }
}

}  // namespace bupp::async_io::bsd_native
