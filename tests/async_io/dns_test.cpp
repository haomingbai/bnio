#include <bupp/async_io/dns.h>

#include <array>
#include <cassert>
#include <cstdint>
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

}  // namespace

int main() {
  static_assert(std::is_same_v<bupp::async_io::dns_result_view::endpoint_type,
                               bupp::async_io::ip::endpoint>);

  test_query_defaults_and_setters();
  test_result_view();
  test_dns_category();
  test_resolve_numeric_v4_endpoint();
  return 0;
}
