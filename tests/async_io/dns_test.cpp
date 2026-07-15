#include <bupp/async_io/dns.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace {

void test_query_defaults_and_setters() {
  bupp::async_io::dns_query query("localhost", "http");
  assert(query.host() == "localhost");
  assert(query.service() == "http");
  assert(query.address_version() ==
         bupp::async_io::ip::address::version::unspecified);
  assert(query.transport() == bupp::async_io::dns_transport::tcp);
  assert(query.flags() == bupp::async_io::dns_query_flags::none);

  assert(query.set_host("127.0.0.1"));
  assert(query.set_port(8080));
  query.set_address_version(bupp::async_io::ip::address::version::v4);
  query.set_transport(bupp::async_io::dns_transport::udp);
  query.set_flags(bupp::async_io::dns_query_flags::numeric_host);

  assert(query.host() == "127.0.0.1");
  assert(query.service() == "8080");
  assert(query.address_version() == bupp::async_io::ip::address::version::v4);
  assert(query.transport() == bupp::async_io::dns_transport::udp);
  assert(query.flags() == bupp::async_io::dns_query_flags::numeric_host);

  bupp::async_io::basic_dns_query<4, 4> small_query("toolong", "80");
  assert(!small_query.valid());
  assert(small_query.set_host("host"));
  assert(small_query.valid());
}

void test_query_capacity_edges_and_empty_view() {
  bupp::async_io::basic_dns_query<4, 1> query;
  assert(query.valid());
  assert(query.view().host == nullptr);
  assert(query.view().service == nullptr);

  assert(query.set_host(""));
  assert(query.set_service("7"));
  assert(query.valid());
  assert(query.view().host == nullptr);
  assert(query.view().service != nullptr);

  assert(!query.set_service("80"));
  assert(!query.valid());
  assert(query.service().empty());
  assert(query.view().service == nullptr);

  assert(query.set_service(""));
  assert(query.valid());
  assert(query.view().service == nullptr);

  assert(!query.set_port(80));
  assert(!query.valid());
  assert(query.service().empty());

  assert(query.set_port(7));
  assert(query.valid());
  assert(query.service() == "7");
}

void test_result_view() {
  using endpoint = bupp::async_io::ip::endpoint;

  std::array<endpoint, 2> static_storage{};
  bupp::async_io::dns_result_view static_view(static_storage);
  assert(static_view.valid());
  assert(static_view.data() == static_storage.data());
  assert(static_view.size() == static_storage.size());

  std::vector<endpoint> dynamic_storage(2);
  bupp::async_io::dns_result_view dynamic_view(dynamic_storage.data(),
                                               dynamic_storage.size());
  assert(dynamic_view.valid());
  assert(dynamic_view.data() == dynamic_storage.data());
  assert(dynamic_view.size() == dynamic_storage.size());
}

void test_dns_category() {
  std::error_code error = bupp::async_io::make_dns_error_code(-2);
  assert(error.category() == bupp::async_io::dns_category());
  assert(std::string_view(error.category().name()) == "bupp.dns");
  assert(!error.message().empty());
}

void test_resolve_rejects_invalid_inputs() {
  std::array<bupp::async_io::ip::endpoint, 2> results{};

  bupp::async_io::basic_dns_query<4, 4> oversized_query("toolong", "80");
  assert(!oversized_query.valid());
  std::size_t count = 7;
  std::error_code error = bupp::async_io::resolve_dns(
      oversized_query, bupp::async_io::dns_result_view(results), count);
  assert(error == std::errc::invalid_argument);
  assert(count == 0);

  bupp::async_io::dns_query valid_query("127.0.0.1", "80");
  count = 7;
  error = bupp::async_io::resolve_dns(
      valid_query, bupp::async_io::dns_result_view(nullptr, 1), count);
  assert(error == std::errc::invalid_argument);
  assert(count == 0);
}

void test_resolve_honors_zero_capacity_output() {
  bupp::async_io::dns_query query("127.0.0.1", "8080");
  query.set_address_version(bupp::async_io::ip::address::version::v4);

  std::size_t count = 7;
  const std::error_code error = bupp::async_io::resolve_dns(
      query, bupp::async_io::dns_result_view(), count);
  assert(!error);
  assert(count == 0);
}

void test_resolve_numeric_v4_endpoint() {
  bupp::async_io::dns_query query("127.0.0.1", "8080");
  query.set_address_version(bupp::async_io::ip::address::version::v4);

  std::array<bupp::async_io::ip::endpoint, 8> results{};
  std::size_t count = 0;
  const std::error_code error = bupp::async_io::resolve_dns(
      query, bupp::async_io::dns_result_view(results), count);
  assert(!error);
  assert(count > 0);

  bool found = false;
  for (std::size_t index = 0; index < count; ++index) {
    const auto& endpoint = results[index];
    found = found || (endpoint.port() == 8080 &&
                      endpoint.address().type() ==
                          bupp::async_io::ip::address::version::v4);
  }
  assert(found);
}

void test_resolve_numeric_flags_and_transports() {
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
  assert(!error);
  assert(count == 1);
  assert(results[0].port() == 8082);
  assert(results[0].address().type() ==
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
  assert(!error);
  assert(count > 0);

  bool found = false;
  for (std::size_t index = 0; index < count; ++index) {
    found = found || (passive_results[index].port() == 8083 &&
                      passive_results[index].address().type() ==
                          bupp::async_io::ip::address::version::v4);
  }
  assert(found);
}

void test_resolve_rejects_non_numeric_host_when_requested() {
  std::array<bupp::async_io::ip::endpoint, 1> results{};
  bupp::async_io::dns_query query("localhost", "8084");
  query.set_flags(bupp::async_io::dns_query_flags::numeric_host |
                  bupp::async_io::dns_query_flags::numeric_service);

  std::size_t count = 7;
  const std::error_code error = bupp::async_io::resolve_dns(
      query, bupp::async_io::dns_result_view(results), count);
  assert(error);
  assert(error.category() == bupp::async_io::dns_category());
  assert(count == 0);
}

}  // namespace

int main() {
  static_assert(std::is_same_v<bupp::async_io::dns_result_view::endpoint_type,
                               bupp::async_io::ip::endpoint>);

  test_query_defaults_and_setters();
  test_query_capacity_edges_and_empty_view();
  test_result_view();
  test_dns_category();
  test_resolve_rejects_invalid_inputs();
  test_resolve_honors_zero_capacity_output();
  test_resolve_numeric_v4_endpoint();
  test_resolve_numeric_flags_and_transports();
  test_resolve_rejects_non_numeric_host_when_requested();
  return 0;
}
