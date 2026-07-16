#include <bupp/async_io/dns.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace {

TEST(DnsTest, resolve_rejects_invalid_inputs) {
  std::array<bupp::async_io::ip::endpoint, 2> results{};

  bupp::async_io::basic_dns_query<4, 4> oversized_query("toolong", "80");
  EXPECT_FALSE(oversized_query.valid());
  std::size_t count = 7;
  std::error_code error = bupp::async_io::resolve_dns(
      oversized_query, bupp::async_io::dns_result_view(results), count);
  EXPECT_EQ(error, make_error_code(std::errc::invalid_argument));
  EXPECT_EQ(count, 0);

  bupp::async_io::dns_query valid_query("127.0.0.1", "80");
  count = 7;
  error = bupp::async_io::resolve_dns(
      valid_query, bupp::async_io::dns_result_view(nullptr, 1), count);
  EXPECT_EQ(error, make_error_code(std::errc::invalid_argument));
  EXPECT_EQ(count, 0);
}

TEST(DnsTest, resolve_honors_zero_capacity_output) {
  bupp::async_io::dns_query query("127.0.0.1", "8080");
  query.set_address_version(bupp::async_io::ip::address::version::v4);

  std::size_t count = 7;
  const std::error_code error = bupp::async_io::resolve_dns(
      query, bupp::async_io::dns_result_view(), count);
  EXPECT_FALSE(error);
  EXPECT_EQ(count, 0);
}

TEST(DnsTest, resolve_numeric_v4_endpoint) {
  bupp::async_io::dns_query query("127.0.0.1", "8080");
  query.set_address_version(bupp::async_io::ip::address::version::v4);

  std::array<bupp::async_io::ip::endpoint, 8> results{};
  std::size_t count = 0;
  const std::error_code error = bupp::async_io::resolve_dns(
      query, bupp::async_io::dns_result_view(results), count);
  EXPECT_FALSE(error);
  EXPECT_GT(count, 0);

  bool found = false;
  for (std::size_t index = 0; index < count; ++index) {
    const auto& endpoint = results[index];
    found = found || (endpoint.port() == 8080 &&
                      endpoint.address().type() ==
                          bupp::async_io::ip::address::version::v4);
  }
  EXPECT_TRUE(found);
}

TEST(DnsTest, resolve_numeric_flags_and_transports) {
  std::array<bupp::async_io::ip::endpoint, 1> results{};
  bupp::async_io::dns_query numeric_query("127.0.0.1", "8082");
  numeric_query.set_address_version(bupp::async_io::ip::address::version::v4);
  numeric_query.set_transport(bupp::async_io::dns_transport::any);
  numeric_query.set_flags(bupp::async_io::dns_query_flags::canonical_name |
                          bupp::async_io::dns_query_flags::numeric_host |
                          bupp::async_io::dns_query_flags::numeric_service);

  std::size_t count = 7;
  std::error_code error = bupp::async_io::resolve_dns(
      numeric_query, bupp::async_io::dns_result_view(results), count);
  EXPECT_FALSE(error);
  EXPECT_EQ(count, 1);
  EXPECT_EQ(results[0].port(), 8082);
  EXPECT_EQ(results[0].address().type(),
            bupp::async_io::ip::address::version::v4);

  std::array<bupp::async_io::ip::endpoint, 4> passive_results{};
  bupp::async_io::dns_query passive_query("", "8083");
  passive_query.set_address_version(bupp::async_io::ip::address::version::v4);
  passive_query.set_transport(bupp::async_io::dns_transport::udp);
  passive_query.set_flags(bupp::async_io::dns_query_flags::passive |
                          bupp::async_io::dns_query_flags::numeric_service);

  count = 7;
  error = bupp::async_io::resolve_dns(
      passive_query, bupp::async_io::dns_result_view(passive_results), count);
  EXPECT_FALSE(error);
  EXPECT_GT(count, 0);

  bool found = false;
  for (std::size_t index = 0; index < count; ++index) {
    found = found || (passive_results[index].port() == 8083 &&
                      passive_results[index].address().type() ==
                          bupp::async_io::ip::address::version::v4);
  }
  EXPECT_TRUE(found);
}

TEST(DnsTest, resolve_rejects_non_numeric_host_when_requested) {
  std::array<bupp::async_io::ip::endpoint, 1> results{};
  bupp::async_io::dns_query query("localhost", "8084");
  query.set_flags(bupp::async_io::dns_query_flags::numeric_host |
                  bupp::async_io::dns_query_flags::numeric_service);

  std::size_t count = 7;
  const std::error_code error = bupp::async_io::resolve_dns(
      query, bupp::async_io::dns_result_view(results), count);
  EXPECT_TRUE(error);
  EXPECT_EQ(error.category(), bupp::async_io::dns_category());
  EXPECT_EQ(count, 0);
}

}  // namespace
