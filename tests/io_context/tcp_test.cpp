#include <bupp/tcp.h>

#include <cassert>
#include <cerrno>
#include <system_error>
#include <type_traits>

namespace {

void test_ip_aliases() {
  static_assert(std::is_same_v<bupp::ip::address, bupp::async_io::ip::address>);
  static_assert(
      std::is_same_v<bupp::ip::endpoint, bupp::async_io::ip::endpoint>);
  static_assert(
      std::is_same_v<bupp::ip::tcp::endpoint, bupp::async_io::ip::endpoint>);
  static_assert(std::is_same_v<bupp::ip::udp, bupp::async_io::ip::udp>);

  const auto address = bupp::ip::make_address("127.0.0.1");
  assert(address.has_value());
  assert(address->is_v4());
}

void test_tcp_type_namespace() {
  static_assert(std::is_same_v<bupp::ip::tcp::socket, bupp::tcp_socket>);
  static_assert(std::is_same_v<bupp::ip::tcp::stream, bupp::tcp_socket>);
  static_assert(std::is_same_v<bupp::ip::tcp::acceptor, bupp::tcp_acceptor>);

  assert(bupp::ip::tcp::v4().version() == bupp::ip::address::version::v4);
  assert(bupp::ip::tcp::v6().version() == bupp::ip::address::version::v6);
  assert(bupp::ip::tcp::v4().async_io_protocol().version() ==
         bupp::async_io::ip::address::version::v4);
}

void test_tcp_owner_aliases() {
  bupp::ip::tcp::socket socket(3);
  assert(socket.native_handle() == 3);
  assert(socket.release() == 3);

  bupp::ip::tcp::acceptor acceptor(4);
  assert(acceptor.native_handle() == 4);
  assert(acceptor.release() == 4);
}

void test_open_protocol_rejects_unspecified_tcp() {
  bupp::ip::tcp::socket socket;
  const std::error_code socket_error = socket.open(bupp::ip::tcp());
  assert(socket_error ==
         std::error_code(EAFNOSUPPORT, std::generic_category()));
  assert(!socket.is_open());

  bupp::ip::tcp::acceptor acceptor;
  const std::error_code acceptor_error = acceptor.open(bupp::ip::tcp());
  assert(acceptor_error ==
         std::error_code(EAFNOSUPPORT, std::generic_category()));
  assert(!acceptor.is_open());
}

}  // namespace

int main() {
  test_ip_aliases();
  test_tcp_type_namespace();
  test_tcp_owner_aliases();
  test_open_protocol_rejects_unspecified_tcp();
  return 0;
}
