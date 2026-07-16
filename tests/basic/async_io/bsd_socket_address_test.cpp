#include <arpa/inet.h>
#include <bupp/async_io/bsd/socket_address.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>

namespace {

using bupp::async_io::bsd_native::make_endpoint;
using bupp::async_io::bsd_native::socket_address;
using bupp::async_io::ip::address;
using bupp::async_io::ip::endpoint;

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

TEST(BsdSocketAddressTest, empty_address) {
  const socket_address native;
  EXPECT_FALSE(native.valid());
  EXPECT_EQ(native.family(), AF_UNSPEC);
  EXPECT_EQ(native.data(), nullptr);
  EXPECT_EQ(native.size(), 0);
}

TEST(BsdSocketAddressTest, v4_address) {
  const socket_address native(endpoint::loopback_v4(8080));
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

TEST(BsdSocketAddressTest, v6_address) {
  const socket_address native(endpoint::loopback_v6(8081));
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

TEST(BsdSocketAddressTest, invalid_native_address) {
  EXPECT_FALSE(make_endpoint(nullptr, 0).has_value());
  const socket_address native(endpoint::loopback_v4(8082));
  EXPECT_FALSE(
      make_endpoint(native.data(), static_cast<socklen_t>(sizeof(sa_family_t)))
          .has_value());

  sockaddr_storage unsupported{};
  auto* raw = reinterpret_cast<sockaddr*>(&unsupported);
  raw->sa_family = AF_UNIX;
  EXPECT_FALSE(make_endpoint(raw, static_cast<socklen_t>(sizeof(unsupported)))
                   .has_value());
}

}  // namespace
