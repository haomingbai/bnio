#include <bnio/tcp.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
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
  static_assert(std::is_same_v<bnio::ip::address, bnio::async_io::ip::address>);
  static_assert(
      std::is_same_v<bnio::ip::endpoint, bnio::async_io::ip::endpoint>);
  static_assert(
      std::is_same_v<bnio::ip::tcp::endpoint, bnio::async_io::ip::endpoint>);
  static_assert(
      std::is_same_v<bnio::ip::udp::endpoint, bnio::async_io::ip::endpoint>);

  const auto address = bnio::ip::make_address("127.0.0.1");
  EXPECT_TRUE(address.has_value());
  EXPECT_TRUE(address->is_v4());
}

TEST(TcpTest, tcp_type_namespace) {
  static_assert(std::is_same_v<bnio::tcp::socket, bnio::tcp_socket>);
  static_assert(std::is_same_v<bnio::tcp::acceptor, bnio::tcp_acceptor>);
  static_assert(std::is_same_v<bnio::ip::tcp::socket, bnio::tcp_socket>);
  static_assert(std::is_same_v<bnio::ip::tcp::stream, bnio::tcp_socket>);
  static_assert(std::is_same_v<bnio::ip::tcp::acceptor, bnio::tcp_acceptor>);

  EXPECT_EQ(bnio::ip::tcp::v4().version(), bnio::ip::address::version::v4);
  EXPECT_EQ(bnio::ip::tcp::v6().version(), bnio::ip::address::version::v6);
  EXPECT_EQ(bnio::ip::tcp::v4().async_io_protocol().version(),
            bnio::async_io::ip::address::version::v4);
}

TEST(TcpTest, tcp_owner_aliases) {
  bnio::ip::tcp::socket socket(3);
  EXPECT_EQ(socket.native_handle(), 3);
  EXPECT_EQ(socket.release(), 3);

  bnio::ip::tcp::acceptor acceptor(4);
  EXPECT_EQ(acceptor.native_handle(), 4);
  EXPECT_EQ(acceptor.release(), 4);
}

TEST(TcpTest, open_protocol_rejects_unspecified_tcp) {
  bnio::ip::tcp::socket socket;
  const std::error_code socket_error = socket.open(bnio::ip::tcp());
  EXPECT_TRUE(socket_error ==
              std::error_code(EAFNOSUPPORT, std::generic_category()));
  EXPECT_FALSE(socket.is_open());

  bnio::ip::tcp::acceptor acceptor;
  const std::error_code acceptor_error = acceptor.open(bnio::ip::tcp());
  EXPECT_TRUE(acceptor_error ==
              std::error_code(EAFNOSUPPORT, std::generic_category()));
  EXPECT_FALSE(acceptor.is_open());
}

TEST(TcpTest, socket_owns_and_replaces_descriptors) {
  bnio::tcp::socket socket;
  EXPECT_FALSE(socket.close());
  EXPECT_FALSE(socket.open(bnio::ip::tcp::v4()));
  const int transferred_fd = socket.native_handle();
  assert_close_on_exec(transferred_fd);

  EXPECT_FALSE(socket.open(bnio::ip::tcp::v4()));
  EXPECT_EQ(socket.native_handle(), transferred_fd);
  self_move_assign(socket);
  EXPECT_EQ(socket.native_handle(), transferred_fd);

  bnio::tcp::socket moved(std::move(socket));
  EXPECT_FALSE(socket.is_open());
  EXPECT_EQ(moved.native_handle(), transferred_fd);

  bnio::tcp::socket destination;
  EXPECT_FALSE(destination.open(bnio::ip::tcp::v4()));
  const int replaced_fd = destination.native_handle();
  destination = std::move(moved);
  EXPECT_FALSE(moved.is_open());
  EXPECT_EQ(destination.native_handle(), transferred_fd);
  assert_descriptor_closed(replaced_fd);

  bnio::tcp::socket replacement;
  EXPECT_FALSE(replacement.open(bnio::ip::tcp::v4()));
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
    bnio::tcp::socket owned;
    EXPECT_FALSE(owned.open(bnio::ip::tcp::v4()));
    owned_fd = owned.native_handle();
  }
  assert_descriptor_closed(owned_fd);

  bnio::tcp::socket externally_closed;
  EXPECT_FALSE(externally_closed.open(bnio::ip::tcp::v4()));
  const int fd = externally_closed.native_handle();
  EXPECT_EQ(::close(fd), 0);
  const std::error_code error = externally_closed.close();
  EXPECT_TRUE(error == std::error_code(EBADF, std::generic_category()));
  EXPECT_FALSE(externally_closed.is_open());

  bnio::tcp::socket invalid_family;
  EXPECT_TRUE(invalid_family.open(AF_UNSPEC));
  EXPECT_FALSE(invalid_family.is_open());
  EXPECT_TRUE(invalid_family.shutdown(SHUT_RDWR) ==
              std::error_code(EBADF, std::generic_category()));
}

TEST(TcpTest, socket_remote_endpoint) {
  bnio::tcp::acceptor listener;
  ASSERT_FALSE(listener.open(AF_INET));
  ASSERT_FALSE(listener.set_reuse_address(true));
  ASSERT_FALSE(listener.bind(bnio::ip::endpoint::loopback_v4(0)));
  ASSERT_FALSE(listener.listen(1));

  // The acceptor opens nonblocking; restore blocking mode so the raw accept
  // below waits for the synchronous connect.
  const int listener_flags = ::fcntl(listener.native_handle(), F_GETFL, 0);
  ASSERT_GE(listener_flags, 0);
  ASSERT_EQ(
      ::fcntl(listener.native_handle(), F_SETFL, listener_flags & ~O_NONBLOCK),
      0);

  sockaddr_in listener_address{};
  socklen_t listener_address_size = sizeof(listener_address);
  ASSERT_EQ(::getsockname(listener.native_handle(),
                          reinterpret_cast<sockaddr*>(&listener_address),
                          &listener_address_size),
            0);
  const bnio::ip::endpoint listener_endpoint(bnio::ip::address::loopback_v4(),
                                             ntohs(listener_address.sin_port));

  // A blocking raw descriptor connects synchronously through the stream
  // socket view; tcp::socket then takes ownership for the endpoint query.
  const int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(client_fd, 0);
  ASSERT_FALSE(
      bnio::async_io::stream_socket_view(client_fd).connect(listener_endpoint));
  bnio::tcp::socket client(client_fd);

  const int accepted_fd = ::accept(listener.native_handle(), nullptr, nullptr);
  ASSERT_GE(accepted_fd, 0);
  bnio::tcp::socket accepted(accepted_fd);

  // The connected client sees the listener endpoint as its remote peer.
  bnio::ip::endpoint peer;
  EXPECT_FALSE(client.remote_endpoint(peer));
  EXPECT_TRUE(peer.address().is_v4());
  EXPECT_EQ(peer.address().to_v4(), bnio::ip::address::loopback_v4().to_v4());
  EXPECT_EQ(peer.port(), listener_endpoint.port());

  // The accepted connection sees the client's ephemeral endpoint as its
  // remote peer, cross-checked against the client's bound address.
  sockaddr_in client_address{};
  socklen_t client_address_size = sizeof(client_address);
  ASSERT_EQ(::getsockname(client.native_handle(),
                          reinterpret_cast<sockaddr*>(&client_address),
                          &client_address_size),
            0);
  bnio::ip::endpoint accepted_peer;
  EXPECT_FALSE(accepted.remote_endpoint(accepted_peer));
  EXPECT_TRUE(accepted_peer.address().is_v4());
  EXPECT_EQ(accepted_peer.port(), ntohs(client_address.sin_port));

  // A closed socket reports EBADF and resets the output endpoint.
  bnio::tcp::socket closed;
  bnio::ip::endpoint stale = bnio::ip::endpoint::loopback_v4(1234);
  EXPECT_EQ(closed.remote_endpoint(stale),
            std::error_code(EBADF, std::generic_category()));
  EXPECT_EQ(stale.version(), bnio::ip::address::version::unspecified);
}

TEST(TcpTest, acceptor_owns_and_replaces_descriptors) {
  bnio::tcp::acceptor acceptor;
  EXPECT_FALSE(acceptor.close());
  EXPECT_FALSE(acceptor.open(bnio::ip::tcp::v4()));
  const int transferred_fd = acceptor.native_handle();
  assert_close_on_exec(transferred_fd);
  EXPECT_FALSE(acceptor.open(bnio::ip::tcp::v4()));
  EXPECT_EQ(acceptor.native_handle(), transferred_fd);
  self_move_assign(acceptor);
  EXPECT_EQ(acceptor.native_handle(), transferred_fd);

  bnio::tcp::acceptor moved(std::move(acceptor));
  EXPECT_FALSE(acceptor.is_open());
  EXPECT_EQ(moved.native_handle(), transferred_fd);

  bnio::tcp::acceptor destination;
  EXPECT_FALSE(destination.open(bnio::ip::tcp::v4()));
  const int replaced_fd = destination.native_handle();
  destination = std::move(moved);
  EXPECT_FALSE(moved.is_open());
  EXPECT_EQ(destination.native_handle(), transferred_fd);
  assert_descriptor_closed(replaced_fd);

  bnio::tcp::acceptor replacement;
  EXPECT_FALSE(replacement.open(bnio::ip::tcp::v4()));
  const int replacement_fd = replacement.release();
  destination.assign(replacement_fd);
  EXPECT_EQ(destination.native_handle(), replacement_fd);
  assert_descriptor_closed(transferred_fd);
  destination.assign(replacement_fd);
  EXPECT_GE(::fcntl(replacement_fd, F_GETFD), 0);
  EXPECT_FALSE(destination.set_reuse_address(true));
  EXPECT_FALSE(destination.close());
  assert_descriptor_closed(replacement_fd);

  bnio::tcp::acceptor invalid_family;
  EXPECT_TRUE(invalid_family.open(AF_UNSPEC));
  EXPECT_FALSE(invalid_family.is_open());
  EXPECT_TRUE(invalid_family.shutdown(SHUT_RDWR) ==
              std::error_code(EBADF, std::generic_category()));
}

TEST(TcpTest, acceptor_destructor_and_close_error) {
  int owned_fd = -1;
  {
    bnio::tcp::acceptor owned;
    EXPECT_FALSE(owned.open(bnio::ip::tcp::v4()));
    owned_fd = owned.native_handle();
  }
  assert_descriptor_closed(owned_fd);

  bnio::tcp::acceptor externally_closed;
  EXPECT_FALSE(externally_closed.open(bnio::ip::tcp::v4()));
  const int fd = externally_closed.native_handle();
  EXPECT_EQ(::close(fd), 0);
  const std::error_code error = externally_closed.close();
  EXPECT_TRUE(error == std::error_code(EBADF, std::generic_category()));
  EXPECT_FALSE(externally_closed.is_open());
}

}  // namespace
