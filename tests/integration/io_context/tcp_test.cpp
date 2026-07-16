#include <bupp/tcp.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

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
  EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}

void assert_close_on_exec(int fd) {
  const int flags = ::fcntl(fd, F_GETFD);
  EXPECT_GE(flags, 0);
  EXPECT_TRUE((flags & FD_CLOEXEC) != 0);
}

TEST(TcpTest, ip_aliases) {
  static_assert(std::is_same_v<bupp::ip::address, bupp::async_io::ip::address>);
  static_assert(
      std::is_same_v<bupp::ip::endpoint, bupp::async_io::ip::endpoint>);
  static_assert(
      std::is_same_v<bupp::ip::tcp::endpoint, bupp::async_io::ip::endpoint>);
  static_assert(
      std::is_same_v<bupp::ip::udp::endpoint, bupp::async_io::ip::endpoint>);

  const auto address = bupp::ip::make_address("127.0.0.1");
  EXPECT_TRUE(address.has_value());
  EXPECT_TRUE(address->is_v4());
}

TEST(TcpTest, tcp_type_namespace) {
  static_assert(std::is_same_v<bupp::tcp::socket, bupp::tcp_socket>);
  static_assert(std::is_same_v<bupp::tcp::acceptor, bupp::tcp_acceptor>);
  static_assert(std::is_same_v<bupp::ip::tcp::socket, bupp::tcp_socket>);
  static_assert(std::is_same_v<bupp::ip::tcp::stream, bupp::tcp_socket>);
  static_assert(std::is_same_v<bupp::ip::tcp::acceptor, bupp::tcp_acceptor>);

  EXPECT_EQ(bupp::ip::tcp::v4().version(), bupp::ip::address::version::v4);
  EXPECT_EQ(bupp::ip::tcp::v6().version(), bupp::ip::address::version::v6);
  EXPECT_EQ(bupp::ip::tcp::v4().async_io_protocol().version(),
            bupp::async_io::ip::address::version::v4);
}

TEST(TcpTest, tcp_owner_aliases) {
  bupp::ip::tcp::socket socket(3);
  EXPECT_EQ(socket.native_handle(), 3);
  EXPECT_EQ(socket.release(), 3);

  bupp::ip::tcp::acceptor acceptor(4);
  EXPECT_EQ(acceptor.native_handle(), 4);
  EXPECT_EQ(acceptor.release(), 4);
}

TEST(TcpTest, open_protocol_rejects_unspecified_tcp) {
  bupp::ip::tcp::socket socket;
  const std::error_code socket_error = socket.open(bupp::ip::tcp());
  EXPECT_TRUE(socket_error ==
              std::error_code(EAFNOSUPPORT, std::generic_category()));
  EXPECT_FALSE(socket.is_open());

  bupp::ip::tcp::acceptor acceptor;
  const std::error_code acceptor_error = acceptor.open(bupp::ip::tcp());
  EXPECT_TRUE(acceptor_error ==
              std::error_code(EAFNOSUPPORT, std::generic_category()));
  EXPECT_FALSE(acceptor.is_open());
}

TEST(TcpTest, socket_owns_and_replaces_descriptors) {
  bupp::tcp::socket socket;
  EXPECT_FALSE(socket.close());
  EXPECT_FALSE(socket.open(bupp::ip::tcp::v4()));
  const int transferred_fd = socket.native_handle();
  assert_close_on_exec(transferred_fd);

  EXPECT_FALSE(socket.open(bupp::ip::tcp::v4()));
  EXPECT_EQ(socket.native_handle(), transferred_fd);
  self_move_assign(socket);
  EXPECT_EQ(socket.native_handle(), transferred_fd);

  bupp::tcp::socket moved(std::move(socket));
  EXPECT_FALSE(socket.is_open());
  EXPECT_EQ(moved.native_handle(), transferred_fd);

  bupp::tcp::socket destination;
  EXPECT_FALSE(destination.open(bupp::ip::tcp::v4()));
  const int replaced_fd = destination.native_handle();
  destination = std::move(moved);
  EXPECT_FALSE(moved.is_open());
  EXPECT_EQ(destination.native_handle(), transferred_fd);
  assert_descriptor_closed(replaced_fd);

  bupp::tcp::socket replacement;
  EXPECT_FALSE(replacement.open(bupp::ip::tcp::v4()));
  const int replacement_fd = replacement.release();
  destination.assign(replacement_fd);
  EXPECT_EQ(destination.native_handle(), replacement_fd);
  assert_descriptor_closed(transferred_fd);
  destination.assign(replacement_fd);
  EXPECT_GE(::fcntl(replacement_fd, F_GETFD), 0);
  EXPECT_FALSE(destination.set_reuse_address(true));
  EXPECT_FALSE(destination.set_reuse_address(false));
  EXPECT_FALSE(destination.close());
  assert_descriptor_closed(replacement_fd);
  EXPECT_FALSE(destination.close());
}

TEST(TcpTest, socket_destructor_and_close_error) {
  int owned_fd = -1;
  {
    bupp::tcp::socket owned;
    EXPECT_FALSE(owned.open(bupp::ip::tcp::v4()));
    owned_fd = owned.native_handle();
  }
  assert_descriptor_closed(owned_fd);

  bupp::tcp::socket externally_closed;
  EXPECT_FALSE(externally_closed.open(bupp::ip::tcp::v4()));
  const int fd = externally_closed.native_handle();
  EXPECT_EQ(::close(fd), 0);
  const std::error_code error = externally_closed.close();
  EXPECT_TRUE(error == std::error_code(EBADF, std::generic_category()));
  EXPECT_FALSE(externally_closed.is_open());

  bupp::tcp::socket invalid_family;
  EXPECT_TRUE(invalid_family.open(AF_UNSPEC));
  EXPECT_FALSE(invalid_family.is_open());
  EXPECT_TRUE(invalid_family.shutdown(SHUT_RDWR) ==
              std::error_code(EBADF, std::generic_category()));
}

TEST(TcpTest, acceptor_owns_and_replaces_descriptors) {
  bupp::tcp::acceptor acceptor;
  EXPECT_FALSE(acceptor.close());
  EXPECT_FALSE(acceptor.open(bupp::ip::tcp::v4()));
  const int transferred_fd = acceptor.native_handle();
  assert_close_on_exec(transferred_fd);
  EXPECT_FALSE(acceptor.open(bupp::ip::tcp::v4()));
  EXPECT_EQ(acceptor.native_handle(), transferred_fd);
  self_move_assign(acceptor);
  EXPECT_EQ(acceptor.native_handle(), transferred_fd);

  bupp::tcp::acceptor moved(std::move(acceptor));
  EXPECT_FALSE(acceptor.is_open());
  EXPECT_EQ(moved.native_handle(), transferred_fd);

  bupp::tcp::acceptor destination;
  EXPECT_FALSE(destination.open(bupp::ip::tcp::v4()));
  const int replaced_fd = destination.native_handle();
  destination = std::move(moved);
  EXPECT_FALSE(moved.is_open());
  EXPECT_EQ(destination.native_handle(), transferred_fd);
  assert_descriptor_closed(replaced_fd);

  bupp::tcp::acceptor replacement;
  EXPECT_FALSE(replacement.open(bupp::ip::tcp::v4()));
  const int replacement_fd = replacement.release();
  destination.assign(replacement_fd);
  EXPECT_EQ(destination.native_handle(), replacement_fd);
  assert_descriptor_closed(transferred_fd);
  destination.assign(replacement_fd);
  EXPECT_GE(::fcntl(replacement_fd, F_GETFD), 0);
  EXPECT_FALSE(destination.set_reuse_address(true));
  EXPECT_FALSE(destination.close());
  assert_descriptor_closed(replacement_fd);

  bupp::tcp::acceptor invalid_family;
  EXPECT_TRUE(invalid_family.open(AF_UNSPEC));
  EXPECT_FALSE(invalid_family.is_open());
  EXPECT_TRUE(invalid_family.shutdown(SHUT_RDWR) ==
              std::error_code(EBADF, std::generic_category()));
}

TEST(TcpTest, acceptor_destructor_and_close_error) {
  int owned_fd = -1;
  {
    bupp::tcp::acceptor owned;
    EXPECT_FALSE(owned.open(bupp::ip::tcp::v4()));
    owned_fd = owned.native_handle();
  }
  assert_descriptor_closed(owned_fd);

  bupp::tcp::acceptor externally_closed;
  EXPECT_FALSE(externally_closed.open(bupp::ip::tcp::v4()));
  const int fd = externally_closed.native_handle();
  EXPECT_EQ(::close(fd), 0);
  const std::error_code error = externally_closed.close();
  EXPECT_TRUE(error == std::error_code(EBADF, std::generic_category()));
  EXPECT_FALSE(externally_closed.is_open());
}

}  // namespace
