#include <arpa/inet.h>
#include <bnio/async_io/linux/socket_address.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>

namespace {

using bnio::async_io::ip::address;
using bnio::async_io::ip::endpoint;
using bnio::async_io::linux_native::make_endpoint;
using bnio::async_io::linux_native::socket_address;

constexpr address::v4_bytes k_loopback_v4{127, 0, 0, 1};
constexpr address::v6_bytes k_loopback_v6{0, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 1};

void check_v4_endpoint(const endpoint& value, std::uint16_t port) {
  EXPECT_EQ(value.version(), address::version::v4);
  EXPECT_EQ(value.port(), port);
  EXPECT_NE(value.address().v4(), nullptr);
  EXPECT_EQ(*value.address().v4(), k_loopback_v4);
}

void check_v6_endpoint(const endpoint& value, std::uint16_t port) {
  EXPECT_EQ(value.version(), address::version::v6);
  EXPECT_EQ(value.port(), port);
  EXPECT_NE(value.address().v6(), nullptr);
  EXPECT_EQ(*value.address().v6(), k_loopback_v6);
}

TEST(LinuxSocketAddressTest, empty_address) {
  const socket_address native;
  EXPECT_FALSE(native.valid());
  EXPECT_EQ(native.family(), AF_UNSPEC);
  EXPECT_EQ(native.data(), nullptr);
  EXPECT_EQ(native.size(), 0);
}

TEST(LinuxSocketAddressTest, unspecified_endpoint_address) {
  socket_address native(endpoint{});
  EXPECT_FALSE(native.valid());
  EXPECT_EQ(native.family(), AF_UNSPEC);
  EXPECT_EQ(native.data(), nullptr);
  EXPECT_EQ(native.size(), 0);
}

TEST(LinuxSocketAddressTest, v4_address) {
  const auto value = endpoint::loopback_v4(8080);
  const socket_address native(value);

  EXPECT_TRUE(native.valid());
  EXPECT_EQ(native.family(), AF_INET);
  EXPECT_EQ(native.size(), sizeof(sockaddr_in));

  const auto* raw = reinterpret_cast<const sockaddr_in*>(native.data());
  EXPECT_EQ(raw->sin_family, AF_INET);
  EXPECT_EQ(ntohs(raw->sin_port), 8080);
  EXPECT_TRUE(std::memcmp(&raw->sin_addr, k_loopback_v4.data(),
                          k_loopback_v4.size()) == 0);

  const auto restored = make_endpoint(native.data(), native.size());
  EXPECT_TRUE(restored.has_value());
  check_v4_endpoint(*restored, 8080);
}

TEST(LinuxSocketAddressTest, mutable_v4_address_data) {
  socket_address native(endpoint::loopback_v4(8084));

  sockaddr* raw = native.data();
  EXPECT_NE(raw, nullptr);
  EXPECT_EQ(raw->sa_family, AF_INET);
}

TEST(LinuxSocketAddressTest, v6_address) {
  const auto value = endpoint::loopback_v6(8081);
  const socket_address native(value);

  EXPECT_TRUE(native.valid());
  EXPECT_EQ(native.family(), AF_INET6);
  EXPECT_EQ(native.size(), sizeof(sockaddr_in6));

  const auto* raw = reinterpret_cast<const sockaddr_in6*>(native.data());
  EXPECT_EQ(raw->sin6_family, AF_INET6);
  EXPECT_EQ(ntohs(raw->sin6_port), 8081);
  EXPECT_TRUE(std::memcmp(&raw->sin6_addr, k_loopback_v6.data(),
                          k_loopback_v6.size()) == 0);

  const auto restored = make_endpoint(native.data(), native.size());
  EXPECT_TRUE(restored.has_value());
  check_v6_endpoint(*restored, 8081);
}

TEST(LinuxSocketAddressTest, invalid_native_address) {
  EXPECT_FALSE(make_endpoint(nullptr, 0).has_value());

  sockaddr_storage too_short{};
  auto* too_short_raw = reinterpret_cast<sockaddr*>(&too_short);
  EXPECT_FALSE(make_endpoint(too_short_raw, 0).has_value());

  const socket_address native(endpoint::loopback_v4(8082));
  EXPECT_FALSE(
      make_endpoint(native.data(), static_cast<socklen_t>(sizeof(sa_family_t)))
          .has_value());

  sockaddr_in6 short_v6{};
  short_v6.sin6_family = static_cast<sa_family_t>(AF_INET6);
  EXPECT_FALSE(make_endpoint(reinterpret_cast<sockaddr*>(&short_v6),
                             static_cast<socklen_t>(sizeof(sa_family_t)))
                   .has_value());

  sockaddr_storage unsupported{};
  auto* unsupported_raw = reinterpret_cast<sockaddr*>(&unsupported);
  unsupported_raw->sa_family = AF_UNIX;
  EXPECT_FALSE(make_endpoint(unsupported_raw,
                             static_cast<socklen_t>(sizeof(unsupported)))
                   .has_value());
}

}  // namespace
