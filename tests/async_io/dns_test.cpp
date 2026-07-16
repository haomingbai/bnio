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
  EXPECT_TRUE(query.host() == "localhost");
  EXPECT_TRUE(query.service() == "http");
  EXPECT_TRUE(query.address_version() ==
              bupp::async_io::ip::address::version::unspecified);
  EXPECT_TRUE(query.transport() == bupp::async_io::dns_transport::tcp);
  EXPECT_TRUE(query.flags() == bupp::async_io::dns_query_flags::none);

  EXPECT_TRUE(query.set_host("127.0.0.1"));
  EXPECT_TRUE(query.set_port(8080));
  query.set_address_version(bupp::async_io::ip::address::version::v4);
  query.set_transport(bupp::async_io::dns_transport::udp);
  query.set_flags(bupp::async_io::dns_query_flags::numeric_host);

  EXPECT_TRUE(query.host() == "127.0.0.1");
  EXPECT_TRUE(query.service() == "8080");
  EXPECT_TRUE(query.address_version() ==
              bupp::async_io::ip::address::version::v4);
  EXPECT_TRUE(query.transport() == bupp::async_io::dns_transport::udp);
  EXPECT_TRUE(query.flags() == bupp::async_io::dns_query_flags::numeric_host);

  bupp::async_io::basic_dns_query<4, 4> small_query("toolong", "80");
  EXPECT_TRUE(!small_query.valid());
  EXPECT_TRUE(small_query.set_host("host"));
  EXPECT_TRUE(small_query.valid());
}

TEST(DnsTest, query_capacity_edges_and_empty_view) {
  bupp::async_io::basic_dns_query<4, 1> query;
  EXPECT_TRUE(query.valid());
  EXPECT_TRUE(query.view().host == nullptr);
  EXPECT_TRUE(query.view().service == nullptr);

  EXPECT_TRUE(query.set_host(""));
  EXPECT_TRUE(query.set_service("7"));
  EXPECT_TRUE(query.valid());
  EXPECT_TRUE(query.view().host == nullptr);
  EXPECT_TRUE(query.view().service != nullptr);

  EXPECT_TRUE(!query.set_service("80"));
  EXPECT_TRUE(!query.valid());
  EXPECT_TRUE(query.service().empty());
  EXPECT_TRUE(query.view().service == nullptr);

  EXPECT_TRUE(query.set_service(""));
  EXPECT_TRUE(query.valid());
  EXPECT_TRUE(query.view().service == nullptr);

  EXPECT_TRUE(!query.set_port(80));
  EXPECT_TRUE(!query.valid());
  EXPECT_TRUE(query.service().empty());

  EXPECT_TRUE(query.set_port(7));
  EXPECT_TRUE(query.valid());
  EXPECT_TRUE(query.service() == "7");
}

TEST(DnsTest, result_view) {
  using endpoint = bupp::async_io::ip::endpoint;

  std::array<endpoint, 2> static_storage{};
  bupp::async_io::dns_result_view static_view(static_storage);
  EXPECT_TRUE(static_view.valid());
  EXPECT_TRUE(static_view.data() == static_storage.data());
  EXPECT_TRUE(static_view.size() == static_storage.size());

  std::vector<endpoint> dynamic_storage(2);
  bupp::async_io::dns_result_view dynamic_view(dynamic_storage.data(),
                                               dynamic_storage.size());
  EXPECT_TRUE(dynamic_view.valid());
  EXPECT_TRUE(dynamic_view.data() == dynamic_storage.data());
  EXPECT_TRUE(dynamic_view.size() == dynamic_storage.size());
}

TEST(DnsTest, dns_category) {
  std::error_code error = bupp::async_io::make_dns_error_code(-2);
  EXPECT_TRUE(error.category() == bupp::async_io::dns_category());
  EXPECT_TRUE(std::string_view(error.category().name()) == "bupp.dns");
  EXPECT_TRUE(!error.message().empty());
}

TEST(DnsTest, resolve_rejects_invalid_inputs) {
  std::array<bupp::async_io::ip::endpoint, 2> results{};

  bupp::async_io::basic_dns_query<4, 4> oversized_query("toolong", "80");
  EXPECT_TRUE(!oversized_query.valid());
  std::size_t count = 7;
  std::error_code error = bupp::async_io::resolve_dns(
      oversized_query, bupp::async_io::dns_result_view(results), count);
  EXPECT_TRUE(error == std::errc::invalid_argument);
  EXPECT_TRUE(count == 0);

  bupp::async_io::dns_query valid_query("127.0.0.1", "80");
  count = 7;
  error = bupp::async_io::resolve_dns(
      valid_query, bupp::async_io::dns_result_view(nullptr, 1), count);
  EXPECT_TRUE(error == std::errc::invalid_argument);
  EXPECT_TRUE(count == 0);
}

TEST(DnsTest, resolve_honors_zero_capacity_output) {
  bupp::async_io::dns_query query("127.0.0.1", "8080");
  query.set_address_version(bupp::async_io::ip::address::version::v4);

  std::size_t count = 7;
  const std::error_code error = bupp::async_io::resolve_dns(
      query, bupp::async_io::dns_result_view(), count);
  EXPECT_TRUE(!error);
  EXPECT_TRUE(count == 0);
}

TEST(DnsTest, resolve_numeric_v4_endpoint) {
  bupp::async_io::dns_query query("127.0.0.1", "8080");
  query.set_address_version(bupp::async_io::ip::address::version::v4);

  std::array<bupp::async_io::ip::endpoint, 8> results{};
  std::size_t count = 0;
  const std::error_code error = bupp::async_io::resolve_dns(
      query, bupp::async_io::dns_result_view(results), count);
  EXPECT_TRUE(!error);
  EXPECT_TRUE(count > 0);

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
  EXPECT_TRUE(!error);
  EXPECT_TRUE(count == 1);
  EXPECT_TRUE(results[0].port() == 8082);
  EXPECT_TRUE(results[0].address().type() ==
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
  EXPECT_TRUE(!error);
  EXPECT_TRUE(count > 0);

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
  EXPECT_TRUE(error.category() == bupp::async_io::dns_category());
  EXPECT_TRUE(count == 0);
}

}  // namespace

TEST(DnsTest, behavior) {
  static_assert(std::is_same_v<bupp::async_io::dns_result_view::endpoint_type,
                               bupp::async_io::ip::endpoint>);
}
