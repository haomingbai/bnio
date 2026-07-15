#include <bupp/tcp.h>
#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <system_error>
#include <type_traits>
#include <utility>

namespace {

template <class Owner>
void self_move_assign(Owner& owner) {
  owner = std::move(owner);
}

void assert_descriptor_closed(int fd) {
  errno = 0;
  assert(::fcntl(fd, F_GETFD) == -1);
  assert(errno == EBADF);
}

void assert_close_on_exec(int fd) {
  const int flags = ::fcntl(fd, F_GETFD);
  assert(flags >= 0);
  assert((flags & FD_CLOEXEC) != 0);
}

void test_ip_aliases() {
  static_assert(std::is_same_v<bupp::ip::address, bupp::async_io::ip::address>);
  static_assert(
      std::is_same_v<bupp::ip::endpoint, bupp::async_io::ip::endpoint>);
  static_assert(
      std::is_same_v<bupp::ip::tcp::endpoint, bupp::async_io::ip::endpoint>);
  static_assert(
      std::is_same_v<bupp::ip::udp::endpoint, bupp::async_io::ip::endpoint>);

  const auto address = bupp::ip::make_address("127.0.0.1");
  assert(address.has_value());
  assert(address->is_v4());
}

void test_tcp_type_namespace() {
  static_assert(std::is_same_v<bupp::tcp::socket, bupp::tcp_socket>);
  static_assert(std::is_same_v<bupp::tcp::acceptor, bupp::tcp_acceptor>);
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

void test_socket_owns_and_replaces_descriptors() {
  bupp::tcp::socket socket;
  assert(!socket.close());
  assert(!socket.open(bupp::ip::tcp::v4()));
  const int transferred_fd = socket.native_handle();
  assert_close_on_exec(transferred_fd);

  assert(!socket.open(bupp::ip::tcp::v4()));
  assert(socket.native_handle() == transferred_fd);
  self_move_assign(socket);
  assert(socket.native_handle() == transferred_fd);

  bupp::tcp::socket moved(std::move(socket));
  assert(!socket.is_open());
  assert(moved.native_handle() == transferred_fd);

  bupp::tcp::socket destination;
  assert(!destination.open(bupp::ip::tcp::v4()));
  const int replaced_fd = destination.native_handle();
  destination = std::move(moved);
  assert(!moved.is_open());
  assert(destination.native_handle() == transferred_fd);
  assert_descriptor_closed(replaced_fd);

  bupp::tcp::socket replacement;
  assert(!replacement.open(bupp::ip::tcp::v4()));
  const int replacement_fd = replacement.release();
  destination.assign(replacement_fd);
  assert(destination.native_handle() == replacement_fd);
  assert_descriptor_closed(transferred_fd);
  destination.assign(replacement_fd);
  assert(::fcntl(replacement_fd, F_GETFD) >= 0);
  assert(!destination.set_reuse_address(true));
  assert(!destination.set_reuse_address(false));
  assert(!destination.close());
  assert_descriptor_closed(replacement_fd);
  assert(!destination.close());
}

void test_socket_destructor_and_close_error() {
  int owned_fd = -1;
  {
    bupp::tcp::socket owned;
    assert(!owned.open(bupp::ip::tcp::v4()));
    owned_fd = owned.native_handle();
  }
  assert_descriptor_closed(owned_fd);

  bupp::tcp::socket externally_closed;
  assert(!externally_closed.open(bupp::ip::tcp::v4()));
  const int fd = externally_closed.native_handle();
  assert(::close(fd) == 0);
  const std::error_code error = externally_closed.close();
  assert(error == std::error_code(EBADF, std::generic_category()));
  assert(!externally_closed.is_open());

  bupp::tcp::socket invalid_family;
  assert(invalid_family.open(AF_UNSPEC));
  assert(!invalid_family.is_open());
  assert(invalid_family.shutdown(SHUT_RDWR) ==
         std::error_code(EBADF, std::generic_category()));
}

void test_acceptor_owns_and_replaces_descriptors() {
  bupp::tcp::acceptor acceptor;
  assert(!acceptor.close());
  assert(!acceptor.open(bupp::ip::tcp::v4()));
  const int transferred_fd = acceptor.native_handle();
  assert_close_on_exec(transferred_fd);
  assert(!acceptor.open(bupp::ip::tcp::v4()));
  assert(acceptor.native_handle() == transferred_fd);
  self_move_assign(acceptor);
  assert(acceptor.native_handle() == transferred_fd);

  bupp::tcp::acceptor moved(std::move(acceptor));
  assert(!acceptor.is_open());
  assert(moved.native_handle() == transferred_fd);

  bupp::tcp::acceptor destination;
  assert(!destination.open(bupp::ip::tcp::v4()));
  const int replaced_fd = destination.native_handle();
  destination = std::move(moved);
  assert(!moved.is_open());
  assert(destination.native_handle() == transferred_fd);
  assert_descriptor_closed(replaced_fd);

  bupp::tcp::acceptor replacement;
  assert(!replacement.open(bupp::ip::tcp::v4()));
  const int replacement_fd = replacement.release();
  destination.assign(replacement_fd);
  assert(destination.native_handle() == replacement_fd);
  assert_descriptor_closed(transferred_fd);
  destination.assign(replacement_fd);
  assert(::fcntl(replacement_fd, F_GETFD) >= 0);
  assert(!destination.set_reuse_address(true));
  assert(!destination.close());
  assert_descriptor_closed(replacement_fd);

  bupp::tcp::acceptor invalid_family;
  assert(invalid_family.open(AF_UNSPEC));
  assert(!invalid_family.is_open());
  assert(invalid_family.shutdown(SHUT_RDWR) ==
         std::error_code(EBADF, std::generic_category()));
}

void test_acceptor_destructor_and_close_error() {
  int owned_fd = -1;
  {
    bupp::tcp::acceptor owned;
    assert(!owned.open(bupp::ip::tcp::v4()));
    owned_fd = owned.native_handle();
  }
  assert_descriptor_closed(owned_fd);

  bupp::tcp::acceptor externally_closed;
  assert(!externally_closed.open(bupp::ip::tcp::v4()));
  const int fd = externally_closed.native_handle();
  assert(::close(fd) == 0);
  const std::error_code error = externally_closed.close();
  assert(error == std::error_code(EBADF, std::generic_category()));
  assert(!externally_closed.is_open());
}

}  // namespace

int main() {
  test_ip_aliases();
  test_tcp_type_namespace();
  test_tcp_owner_aliases();
  test_open_protocol_rejects_unspecified_tcp();
  test_socket_owns_and_replaces_descriptors();
  test_socket_destructor_and_close_error();
  test_acceptor_owns_and_replaces_descriptors();
  test_acceptor_destructor_and_close_error();
  return 0;
}
