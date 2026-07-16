#include <bupp/async_io/ip/tcp.h>
#include <bupp/async_io/ip/udp.h>
#include <gtest/gtest.h>

#include <type_traits>

namespace {

using bupp::async_io::ip::address;

void check_v4_endpoint(const bupp::async_io::ip::tcp::endpoint& endpoint,
                       std::uint16_t port, address::v4_bytes expected_address) {
  EXPECT_EQ(endpoint.version(), address::version::v4);
  EXPECT_EQ(endpoint.port(), port);
  EXPECT_TRUE(endpoint.address().is_v4());
  EXPECT_NE(endpoint.address().v4(), nullptr);
  EXPECT_EQ(*endpoint.address().v4(), expected_address);
}

void check_v6_endpoint(const bupp::async_io::ip::tcp::endpoint& endpoint,
                       std::uint16_t port, address::v6_bytes expected_address) {
  EXPECT_EQ(endpoint.version(), address::version::v6);
  EXPECT_EQ(endpoint.port(), port);
  EXPECT_TRUE(endpoint.address().is_v6());
  EXPECT_NE(endpoint.address().v6(), nullptr);
  EXPECT_EQ(*endpoint.address().v6(), expected_address);
}

}  // namespace

TEST(TcpEndpointTest, behavior) {
  constexpr address::v4_bytes k_any_v4{0, 0, 0, 0};
  constexpr address::v4_bytes k_loopback_v4{127, 0, 0, 1};
  constexpr address::v6_bytes k_any_v6{};
  constexpr address::v6_bytes k_loopback_v6{0, 0, 0, 0, 0, 0, 0, 0,
                                            0, 0, 0, 0, 0, 0, 0, 1};

  static_assert(
      std::is_nothrow_move_constructible_v<bupp::async_io::ip::tcp::endpoint>);
  static_assert(
      std::is_nothrow_move_assignable_v<bupp::async_io::ip::tcp::endpoint>);

  EXPECT_EQ(bupp::async_io::ip::tcp::v4().version(), address::version::v4);
  EXPECT_EQ(bupp::async_io::ip::tcp::v6().version(), address::version::v6);
  EXPECT_EQ(bupp::async_io::ip::udp::v4().version(), address::version::v4);
  EXPECT_EQ(bupp::async_io::ip::udp::v6().version(), address::version::v6);

  check_v4_endpoint(bupp::async_io::ip::tcp::endpoint::loopback_v4(8080), 8080,
                    k_loopback_v4);
  check_v4_endpoint(bupp::async_io::ip::tcp::endpoint::any_v4(8081), 8081,
                    k_any_v4);
  check_v6_endpoint(bupp::async_io::ip::tcp::endpoint::loopback_v6(8082), 8082,
                    k_loopback_v6);
  check_v6_endpoint(bupp::async_io::ip::tcp::endpoint::any_v6(8083), 8083,
                    k_any_v6);

  bupp::async_io::ip::tcp::endpoint endpoint(
      bupp::async_io::ip::address::loopback_v4(), 9000);
  endpoint.set_port(9001);
  endpoint.set_v4_address(k_any_v4);
  check_v4_endpoint(endpoint, 9001, k_any_v4);

  endpoint.set_address(bupp::async_io::ip::address::loopback_v6());
  endpoint.set_port(9002);
  check_v6_endpoint(endpoint, 9002, k_loopback_v6);

  endpoint.set_v4_address(k_any_v4);
  check_v6_endpoint(endpoint, 9002, k_loopback_v6);
  endpoint.set_v6_address(k_any_v6);
  check_v6_endpoint(endpoint, 9002, k_any_v6);

  endpoint.set_address(bupp::async_io::ip::address{});
  EXPECT_EQ(endpoint.version(), address::version::unspecified);
  EXPECT_EQ(endpoint.port(), 0);
  endpoint.set_port(9003);
  endpoint.set_v4_address(k_loopback_v4);
  endpoint.set_v6_address(k_loopback_v6);
  EXPECT_EQ(endpoint.version(), address::version::unspecified);
  EXPECT_EQ(endpoint.port(), 0);

  endpoint.set_address(bupp::async_io::ip::address::loopback_v4());
  endpoint.set_port(9004);
  endpoint.set_v6_address(k_loopback_v6);
  check_v4_endpoint(endpoint, 9004, k_loopback_v4);

  endpoint.reset();
  EXPECT_EQ(endpoint.version(), address::version::unspecified);
  EXPECT_EQ(endpoint.port(), 0);
}
