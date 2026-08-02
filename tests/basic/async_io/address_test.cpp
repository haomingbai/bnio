#include <bnio/async_io/ip/address.h>
#include <gtest/gtest.h>

#include <type_traits>

TEST(AddressTest, behavior) {
  using bnio::async_io::ip::address;

  constexpr address::v4_bytes k_any_v4{0, 0, 0, 0};
  constexpr address::v4_bytes k_loopback_v4{127, 0, 0, 1};
  constexpr address::v6_bytes k_loopback_v6{0, 0, 0, 0, 0, 0, 0, 0,
                                            0, 0, 0, 0, 0, 0, 0, 1};

  static_assert(
      std::is_nothrow_move_constructible_v<bnio::async_io::ip::address>);
  static_assert(std::is_nothrow_move_assignable_v<bnio::async_io::ip::address>);

  auto any_v4 = bnio::async_io::ip::address::any_v4();
  EXPECT_TRUE(any_v4.is_v4());
  EXPECT_FALSE(any_v4.is_v6());
  EXPECT_EQ(any_v4.to_v4(), 0);
  EXPECT_NE(any_v4.v4(), nullptr);
  EXPECT_EQ(*any_v4.v4(), k_any_v4);
  EXPECT_EQ(any_v4.v6(), nullptr);

  auto loopback_v4 = bnio::async_io::ip::address::loopback_v4();
  EXPECT_EQ(loopback_v4.to_v4(), 0x7f000001U);
  EXPECT_NE(loopback_v4.v4(), nullptr);
  EXPECT_EQ(*loopback_v4.v4(), k_loopback_v4);

  // Cover address::v4(uint32_t) and host_order_to_v4_bytes/set_v4(uint32_t).
  auto uint_v4 = bnio::async_io::ip::address::v4(0x7f000001U);
  EXPECT_TRUE(uint_v4.is_v4());
  EXPECT_EQ(uint_v4.to_v4(), 0x7f000001U);

  constexpr address::v4_bytes k_google_dns{8, 8, 8, 8};
  auto google_v4 = bnio::async_io::ip::address::v4(k_google_dns);
  EXPECT_TRUE(google_v4.is_v4());
  EXPECT_EQ(google_v4.to_v4(), 0x08080808U);
  EXPECT_EQ(*google_v4.v4(), k_google_dns);

  auto loopback_v6 = bnio::async_io::ip::address::loopback_v6();
  EXPECT_TRUE(loopback_v6.is_v6());
  EXPECT_EQ(loopback_v6.v4(), nullptr);
  EXPECT_NE(loopback_v6.v6(), nullptr);
  EXPECT_EQ(*loopback_v6.v6(), k_loopback_v6);

#if defined(BNIO_HAS_ASYNC_IO_IP_ADDRESS_PARSER)
  auto parsed_v4 = bnio::async_io::ip::make_address("127.0.0.1");
  EXPECT_TRUE(parsed_v4.has_value());
  EXPECT_TRUE(parsed_v4->is_v4());
  EXPECT_EQ(parsed_v4->to_v4(), 0x7f000001U);

  auto parsed_v4_alias = bnio::async_io::ip::make_addr("0.0.0.0");
  EXPECT_TRUE(parsed_v4_alias.has_value());
  EXPECT_EQ(parsed_v4_alias->to_v4(), 0);

  auto parsed_v6 = bnio::async_io::ip::make_v6_address("::1");
  EXPECT_TRUE(parsed_v6.has_value());
  EXPECT_TRUE(parsed_v6->is_v6());
  EXPECT_NE(parsed_v6->v6(), nullptr);
  EXPECT_EQ(*parsed_v6->v6(), k_loopback_v6);

  EXPECT_FALSE(bnio::async_io::ip::make_address("not an address").has_value());
#endif

  bnio::async_io::ip::address reset_address =
      bnio::async_io::ip::address::loopback_v4();
  reset_address.reset();
  EXPECT_EQ(reset_address.type(),
            bnio::async_io::ip::address::version::unspecified);
  EXPECT_EQ(reset_address.v4(), nullptr);
  EXPECT_EQ(reset_address.v6(), nullptr);
}
