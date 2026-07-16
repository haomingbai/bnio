#include <bupp/async_io/dns.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace {

TEST(DnsTest, query_defaults_and_setters) {
  bupp::async_io::dns_query query("localhost", "http");
  EXPECT_EQ(query.host(), "localhost");
  EXPECT_EQ(query.service(), "http");
  EXPECT_EQ(query.address_version(),
            bupp::async_io::ip::address::version::unspecified);
  EXPECT_EQ(query.transport(), bupp::async_io::dns_transport::tcp);
  EXPECT_EQ(query.flags(), bupp::async_io::dns_query_flags::none);

  EXPECT_TRUE(query.set_host("127.0.0.1"));
  EXPECT_TRUE(query.set_port(8080));
  query.set_address_version(bupp::async_io::ip::address::version::v4);
  query.set_transport(bupp::async_io::dns_transport::udp);
  query.set_flags(bupp::async_io::dns_query_flags::numeric_host);

  EXPECT_EQ(query.host(), "127.0.0.1");
  EXPECT_EQ(query.service(), "8080");
  EXPECT_EQ(query.address_version(),
            bupp::async_io::ip::address::version::v4);
  EXPECT_EQ(query.transport(), bupp::async_io::dns_transport::udp);
  EXPECT_EQ(query.flags(), bupp::async_io::dns_query_flags::numeric_host);

  bupp::async_io::basic_dns_query<4, 4> small_query("toolong", "80");
  EXPECT_FALSE(small_query.valid());
  EXPECT_TRUE(small_query.set_host("host"));
  EXPECT_TRUE(small_query.valid());
}

TEST(DnsTest, query_capacity_edges_and_empty_view) {
  bupp::async_io::basic_dns_query<4, 1> query;
  EXPECT_TRUE(query.valid());
  EXPECT_EQ(query.view().host, nullptr);
  EXPECT_EQ(query.view().service, nullptr);

  EXPECT_TRUE(query.set_host(""));
  EXPECT_TRUE(query.set_service("7"));
  EXPECT_TRUE(query.valid());
  EXPECT_EQ(query.view().host, nullptr);
  EXPECT_NE(query.view().service, nullptr);

  EXPECT_FALSE(query.set_service("80"));
  EXPECT_FALSE(query.valid());
  EXPECT_TRUE(query.service().empty());
  EXPECT_EQ(query.view().service, nullptr);

  EXPECT_TRUE(query.set_service(""));
  EXPECT_TRUE(query.valid());
  EXPECT_EQ(query.view().service, nullptr);

  EXPECT_FALSE(query.set_port(80));
  EXPECT_FALSE(query.valid());
  EXPECT_TRUE(query.service().empty());

  EXPECT_TRUE(query.set_port(7));
  EXPECT_TRUE(query.valid());
  EXPECT_EQ(query.service(), "7");
}

TEST(DnsTest, result_view) {
  using endpoint = bupp::async_io::ip::endpoint;

  std::array<endpoint, 2> static_storage{};
  bupp::async_io::dns_result_view static_view(static_storage);
  EXPECT_TRUE(static_view.valid());
  EXPECT_EQ(static_view.data(), static_storage.data());
  EXPECT_EQ(static_view.size(), static_storage.size());

  std::vector<endpoint> dynamic_storage(2);
  bupp::async_io::dns_result_view dynamic_view(dynamic_storage.data(),
                                               dynamic_storage.size());
  EXPECT_TRUE(dynamic_view.valid());
  EXPECT_EQ(dynamic_view.data(), dynamic_storage.data());
  EXPECT_EQ(dynamic_view.size(), dynamic_storage.size());
}

TEST(DnsTest, dns_category) {
  std::error_code error = bupp::async_io::make_dns_error_code(-2);
  EXPECT_EQ(error.category(), bupp::async_io::dns_category());
  EXPECT_EQ(std::string_view(error.category().name()), "bupp.dns");
  EXPECT_FALSE(error.message().empty());
}

}  // namespace

TEST(DnsTest, behavior) {
  static_assert(std::is_same_v<bupp::async_io::dns_result_view::endpoint_type,
                               bupp::async_io::ip::endpoint>);
}
