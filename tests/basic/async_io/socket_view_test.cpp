#include <arpa/inet.h>
#include <bnio/async_io/buffer_view.h>
#include <bnio/async_io/ip/tcp.h>
#include <bnio/async_io/socket_view.h>
#include <bnio/config/system.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <type_traits>
#include <utility>

#if !defined(SOCK_CLOEXEC)
#define SOCK_CLOEXEC 0
#endif

namespace {

template <class Socket, class... Args>
concept can_bind = requires(Socket socket, Args&&... args) {
  socket.bind(std::forward<Args>(args)...);
};

template <class Socket, class... Args>
concept can_connect = requires(Socket socket, Args&&... args) {
  socket.connect(std::forward<Args>(args)...);
};

template <class Socket, class... Args>
concept can_listen = requires(Socket socket, Args&&... args) {
  socket.listen(std::forward<Args>(args)...);
};

template <class Socket>
concept has_sync_datagram_io =
    requires(Socket socket, bnio::async_io::buffer_view buffer,
             bnio::async_io::ip::endpoint endpoint, std::error_code error) {
      socket.send(buffer, 0, error);
      socket.receive(buffer, 0, error);
      socket.send_to(buffer, endpoint, 0, error);
      socket.receive_from(buffer, endpoint, 0, error);
    };

class unique_fd {
 public:
  explicit unique_fd(int fd) noexcept : fd_(fd) {}

  unique_fd(const unique_fd&) = delete;
  unique_fd& operator=(const unique_fd&) = delete;

  ~unique_fd() { reset(); }

  [[nodiscard]] int get() const noexcept { return fd_; }

  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      static_cast<void>(::close(fd_));
    }
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

unique_fd make_tcp_socket() {
  return unique_fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
}

void check_error(const std::error_code& error, int value) {
  EXPECT_TRUE(error);
  EXPECT_EQ(error.value(), value);
  EXPECT_EQ(error.category(), std::generic_category());
}

TEST(SocketViewTest, invalid_socket) {
  bnio::async_io::socket_view generic_socket;
  EXPECT_FALSE(generic_socket.valid());
  EXPECT_EQ(generic_socket.native_handle(), -1);

  const auto endpoint = bnio::async_io::ip::tcp::endpoint::loopback_v4(0);

  bnio::async_io::stream_socket_view stream;
  EXPECT_FALSE(stream.valid());
  EXPECT_EQ(stream.native_handle(), -1);
  check_error(stream.bind(endpoint), EBADF);
  check_error(stream.listen(1), EBADF);
  check_error(stream.connect(endpoint), EBADF);
  check_error(stream.shutdown(SHUT_RDWR), EBADF);
  check_error(stream.set_reuse_address(true), EBADF);

  bnio::async_io::datagram_socket_view datagram;
  EXPECT_FALSE(datagram.valid());
  EXPECT_EQ(datagram.native_handle(), -1);
  check_error(datagram.bind(endpoint), EBADF);
  check_error(datagram.connect(endpoint), EBADF);
  check_error(datagram.shutdown(SHUT_RDWR), EBADF);
  check_error(datagram.set_reuse_address(true), EBADF);

  bnio::async_io::ip::endpoint stale =
      bnio::async_io::ip::endpoint::loopback_v4(1234);
  check_error(datagram.local_endpoint(stale), EBADF);
  EXPECT_EQ(stale.version(), bnio::async_io::ip::address::version::unspecified);
  stale = bnio::async_io::ip::endpoint::loopback_v4(1234);
  check_error(datagram.remote_endpoint(stale), EBADF);
  EXPECT_EQ(stale.version(), bnio::async_io::ip::address::version::unspecified);
}

TEST(SocketViewTest, datagram_lifecycle) {
  unique_fd receiver_fd(
      ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP));
  unique_fd sender_fd(
      ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP));
  EXPECT_TRUE(receiver_fd.get() >= 0);
  EXPECT_TRUE(sender_fd.get() >= 0);

  bnio::async_io::datagram_socket_view receiver(receiver_fd.get());
  bnio::async_io::datagram_socket_view sender(sender_fd.get());
  EXPECT_FALSE(receiver.bind(bnio::async_io::ip::endpoint::loopback_v4(0)));
  bnio::async_io::ip::endpoint destination;
  EXPECT_FALSE(receiver.local_endpoint(destination));
  EXPECT_FALSE(sender.connect(destination));
  bnio::async_io::ip::endpoint peer;
  EXPECT_FALSE(sender.remote_endpoint(peer));
  EXPECT_TRUE(peer.address().is_v4());
  EXPECT_EQ(peer.port(), destination.port());
}

TEST(SocketViewTest, loopback_setup) {
  unique_fd listener_fd = make_tcp_socket();
  EXPECT_TRUE(listener_fd.get() >= 0);

  bnio::async_io::stream_socket_view listener(listener_fd.get());
  EXPECT_TRUE(listener.valid());
  EXPECT_EQ(listener.native_handle(), listener_fd.get());

  EXPECT_FALSE(listener.set_reuse_address(true));
  EXPECT_FALSE(
      listener.bind(bnio::async_io::ip::tcp::endpoint::loopback_v4(0)));
  EXPECT_FALSE(listener.listen(1));

  sockaddr_in bound_address{};
  socklen_t bound_address_size = sizeof(bound_address);
  EXPECT_EQ(::getsockname(listener.native_handle(),
                          reinterpret_cast<sockaddr*>(&bound_address),
                          &bound_address_size),
            0);
  EXPECT_EQ(bound_address.sin_family, AF_INET);
  EXPECT_EQ(bound_address_size, sizeof(bound_address));

  bnio::async_io::ip::tcp::endpoint remote(
      bnio::async_io::ip::address::loopback_v4(),
      ntohs(bound_address.sin_port));

  unique_fd client_fd = make_tcp_socket();
  EXPECT_TRUE(client_fd.get() >= 0);

  bnio::async_io::stream_socket_view client(client_fd.get());
  EXPECT_FALSE(client.connect(remote));

#if defined(BNIO_SYSTEM_LINUX) || defined(BNIO_SYSTEM_FREEBSD)
  unique_fd accepted_fd(
      ::accept4(listener.native_handle(), nullptr, nullptr, SOCK_CLOEXEC));
#else
  unique_fd accepted_fd(::accept(listener.native_handle(), nullptr, nullptr));
#endif
  EXPECT_TRUE(accepted_fd.get() >= 0);

  bnio::async_io::stream_socket_view accepted(accepted_fd.get());
  EXPECT_FALSE(accepted.shutdown(SHUT_RDWR));
}

}  // namespace

TEST(SocketViewTest, behavior) {
  static_assert(sizeof(bnio::async_io::socket_view) == sizeof(int));
  static_assert(std::is_trivially_copyable_v<bnio::async_io::socket_view>);
  static_assert(std::is_standard_layout_v<bnio::async_io::socket_view>);
  static_assert(sizeof(bnio::async_io::stream_socket_view) == sizeof(int));
  static_assert(
      std::is_trivially_copyable_v<bnio::async_io::stream_socket_view>);
  static_assert(std::is_standard_layout_v<bnio::async_io::stream_socket_view>);
  static_assert(sizeof(bnio::async_io::datagram_socket_view) == sizeof(int));
  static_assert(
      std::is_trivially_copyable_v<bnio::async_io::datagram_socket_view>);
  static_assert(
      std::is_standard_layout_v<bnio::async_io::datagram_socket_view>);
  static_assert(can_bind<bnio::async_io::stream_socket_view,
                         const bnio::async_io::ip::endpoint&>);
  static_assert(can_connect<bnio::async_io::stream_socket_view,
                            const bnio::async_io::ip::endpoint&>);
  static_assert(can_listen<bnio::async_io::stream_socket_view, int>);
  static_assert(can_bind<bnio::async_io::datagram_socket_view,
                         const bnio::async_io::ip::endpoint&>);
  static_assert(can_connect<bnio::async_io::datagram_socket_view,
                            const bnio::async_io::ip::endpoint&>);
  static_assert(!can_bind<bnio::async_io::stream_socket_view, const sockaddr*,
                          socklen_t>);
  static_assert(!can_connect<bnio::async_io::stream_socket_view,
                             const sockaddr*, socklen_t>);
  static_assert(!can_listen<bnio::async_io::datagram_socket_view, int>);
  static_assert(!has_sync_datagram_io<bnio::async_io::datagram_socket_view>);
}
