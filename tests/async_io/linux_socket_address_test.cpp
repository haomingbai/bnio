#include <arpa/inet.h>
#include <bupp/async_io/linux/socket_address.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cassert>
#include <cstring>

namespace {

using bupp::async_io::ip::address;
using bupp::async_io::ip::endpoint;
using bupp::async_io::linux_native::make_endpoint;
using bupp::async_io::linux_native::socket_address;

constexpr address::v4_bytes k_loopback_v4{127, 0, 0, 1};
constexpr address::v6_bytes k_loopback_v6{0, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 1};

void check_v4_endpoint(const endpoint& value, std::uint16_t port) {
  assert(value.version() == address::version::v4);
  assert(value.port() == port);
  assert(value.address().v4() != nullptr);
  assert(*value.address().v4() == k_loopback_v4);
}

void check_v6_endpoint(const endpoint& value, std::uint16_t port) {
  assert(value.version() == address::version::v6);
  assert(value.port() == port);
  assert(value.address().v6() != nullptr);
  assert(*value.address().v6() == k_loopback_v6);
}

void test_empty_address() {
  const socket_address native;
  assert(!native.valid());
  assert(native.family() == AF_UNSPEC);
  assert(native.data() == nullptr);
  assert(native.size() == 0);
}

void test_unspecified_endpoint_address() {
  socket_address native(endpoint{});
  assert(!native.valid());
  assert(native.family() == AF_UNSPEC);
  assert(native.data() == nullptr);
  assert(native.size() == 0);
}

void test_v4_address() {
  const auto value = endpoint::loopback_v4(8080);
  const socket_address native(value);

  assert(native.valid());
  assert(native.family() == AF_INET);
  assert(native.size() == sizeof(sockaddr_in));

  const auto* raw = reinterpret_cast<const sockaddr_in*>(native.data());
  assert(raw->sin_family == AF_INET);
  assert(ntohs(raw->sin_port) == 8080);
  assert(std::memcmp(&raw->sin_addr, k_loopback_v4.data(),
                     k_loopback_v4.size()) == 0);

  const auto restored = make_endpoint(native.data(), native.size());
  assert(restored.has_value());
  check_v4_endpoint(*restored, 8080);
}

void test_mutable_v4_address_data() {
  socket_address native(endpoint::loopback_v4(8084));

  sockaddr* raw = native.data();
  assert(raw != nullptr);
  assert(raw->sa_family == AF_INET);
}

void test_v6_address() {
  const auto value = endpoint::loopback_v6(8081);
  const socket_address native(value);

  assert(native.valid());
  assert(native.family() == AF_INET6);
  assert(native.size() == sizeof(sockaddr_in6));

  const auto* raw = reinterpret_cast<const sockaddr_in6*>(native.data());
  assert(raw->sin6_family == AF_INET6);
  assert(ntohs(raw->sin6_port) == 8081);
  assert(std::memcmp(&raw->sin6_addr, k_loopback_v6.data(),
                     k_loopback_v6.size()) == 0);

  const auto restored = make_endpoint(native.data(), native.size());
  assert(restored.has_value());
  check_v6_endpoint(*restored, 8081);
}

void test_invalid_native_address() {
  assert(!make_endpoint(nullptr, 0).has_value());

  sockaddr_storage too_short{};
  auto* too_short_raw = reinterpret_cast<sockaddr*>(&too_short);
  assert(!make_endpoint(too_short_raw, 0).has_value());

  const socket_address native(endpoint::loopback_v4(8082));
  assert(
      !make_endpoint(native.data(), static_cast<socklen_t>(sizeof(sa_family_t)))
           .has_value());

  sockaddr_in6 short_v6{};
  short_v6.sin6_family = static_cast<sa_family_t>(AF_INET6);
  assert(!make_endpoint(reinterpret_cast<sockaddr*>(&short_v6),
                        static_cast<socklen_t>(sizeof(sa_family_t)))
              .has_value());

  sockaddr_storage unsupported{};
  auto* unsupported_raw = reinterpret_cast<sockaddr*>(&unsupported);
  unsupported_raw->sa_family = AF_UNIX;
  assert(!make_endpoint(unsupported_raw,
                        static_cast<socklen_t>(sizeof(unsupported)))
              .has_value());
}

}  // namespace

int main() {
  test_empty_address();
  test_unspecified_endpoint_address();
  test_v4_address();
  test_mutable_v4_address_data();
  test_v6_address();
  test_invalid_native_address();

  return 0;
}
