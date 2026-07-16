#include <arpa/inet.h>
#include <bupp/async_io/buffer_view.h>
#include <bupp/async_io/ip/tcp.h>
#include <bupp/async_io/socket_view.h>
#include <bupp/config/system.h>
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
    requires(Socket socket, bupp::async_io::buffer_view buffer,
             bupp::async_io::ip::endpoint endpoint, std::error_code error) {
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
  EXPECT_TRUE(error.value() == value);
  EXPECT_TRUE(error.category() == std::generic_category());
}

TEST(SocketViewTest, invalid_socket) {
  bupp::async_io::socket_view generic_socket;
  EXPECT_TRUE(!generic_socket.valid());
  EXPECT_TRUE(generic_socket.native_handle() == -1);

  const auto endpoint = bupp::async_io::ip::tcp::endpoint::loopback_v4(0);

  bupp::async_io::stream_socket_view stream;
  EXPECT_TRUE(!stream.valid());
  EXPECT_TRUE(stream.native_handle() == -1);
  check_error(stream.bind(endpoint), EBADF);
  check_error(stream.listen(1), EBADF);
  check_error(stream.connect(endpoint), EBADF);
  check_error(stream.shutdown(SHUT_RDWR), EBADF);
  check_error(stream.set_reuse_address(true), EBADF);

  bupp::async_io::datagram_socket_view datagram;
  EXPECT_TRUE(!datagram.valid());
  EXPECT_TRUE(datagram.native_handle() == -1);
  check_error(datagram.bind(endpoint), EBADF);
  check_error(datagram.connect(endpoint), EBADF);
  check_error(datagram.shutdown(SHUT_RDWR), EBADF);
  check_error(datagram.set_reuse_address(true), EBADF);

  bupp::async_io::ip::endpoint stale =
      bupp::async_io::ip::endpoint::loopback_v4(1234);
  check_error(datagram.local_endpoint(stale), EBADF);
  EXPECT_TRUE(stale.version() ==
              bupp::async_io::ip::address::version::unspecified);
  stale = bupp::async_io::ip::endpoint::loopback_v4(1234);
  check_error(datagram.remote_endpoint(stale), EBADF);
  EXPECT_TRUE(stale.version() ==
              bupp::async_io::ip::address::version::unspecified);
}

TEST(SocketViewTest, datagram_lifecycle) {
  unique_fd receiver_fd(
      ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP));
  unique_fd sender_fd(
      ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP));
  EXPECT_TRUE(receiver_fd.get() >= 0);
  EXPECT_TRUE(sender_fd.get() >= 0);

  bupp::async_io::datagram_socket_view receiver(receiver_fd.get());
  bupp::async_io::datagram_socket_view sender(sender_fd.get());
  EXPECT_TRUE(!receiver.bind(bupp::async_io::ip::endpoint::loopback_v4(0)));
  bupp::async_io::ip::endpoint destination;
  EXPECT_TRUE(!receiver.local_endpoint(destination));
  EXPECT_TRUE(!sender.connect(destination));
  bupp::async_io::ip::endpoint peer;
  EXPECT_TRUE(!sender.remote_endpoint(peer));
  EXPECT_TRUE(peer.address().is_v4());
  EXPECT_TRUE(peer.port() == destination.port());
}

TEST(SocketViewTest, loopback_setup) {
  unique_fd listener_fd = make_tcp_socket();
  EXPECT_TRUE(listener_fd.get() >= 0);

  bupp::async_io::stream_socket_view listener(listener_fd.get());
  EXPECT_TRUE(listener.valid());
  EXPECT_TRUE(listener.native_handle() == listener_fd.get());

  EXPECT_TRUE(!listener.set_reuse_address(true));
  EXPECT_TRUE(
      !listener.bind(bupp::async_io::ip::tcp::endpoint::loopback_v4(0)));
  EXPECT_TRUE(!listener.listen(1));

  sockaddr_in bound_address{};
  socklen_t bound_address_size = sizeof(bound_address);
  EXPECT_TRUE(::getsockname(listener.native_handle(),
                            reinterpret_cast<sockaddr*>(&bound_address),
                            &bound_address_size) == 0);
  EXPECT_TRUE(bound_address.sin_family == AF_INET);
  EXPECT_TRUE(bound_address_size == sizeof(bound_address));

  bupp::async_io::ip::tcp::endpoint remote(
      bupp::async_io::ip::address::loopback_v4(),
      ntohs(bound_address.sin_port));

  unique_fd client_fd = make_tcp_socket();
  EXPECT_TRUE(client_fd.get() >= 0);

  bupp::async_io::stream_socket_view client(client_fd.get());
  EXPECT_TRUE(!client.connect(remote));

#if defined(BUPP_SYSTEM_LINUX) || defined(BUPP_SYSTEM_FREEBSD)
  unique_fd accepted_fd(
      ::accept4(listener.native_handle(), nullptr, nullptr, SOCK_CLOEXEC));
#else
  unique_fd accepted_fd(::accept(listener.native_handle(), nullptr, nullptr));
#endif
  EXPECT_TRUE(accepted_fd.get() >= 0);

  bupp::async_io::stream_socket_view accepted(accepted_fd.get());
  EXPECT_TRUE(!accepted.shutdown(SHUT_RDWR));
}

}  // namespace

TEST(SocketViewTest, behavior) {
  static_assert(sizeof(bupp::async_io::socket_view) == sizeof(int));
  static_assert(std::is_trivially_copyable_v<bupp::async_io::socket_view>);
  static_assert(std::is_standard_layout_v<bupp::async_io::socket_view>);
  static_assert(sizeof(bupp::async_io::stream_socket_view) == sizeof(int));
  static_assert(
      std::is_trivially_copyable_v<bupp::async_io::stream_socket_view>);
  static_assert(std::is_standard_layout_v<bupp::async_io::stream_socket_view>);
  static_assert(sizeof(bupp::async_io::datagram_socket_view) == sizeof(int));
  static_assert(
      std::is_trivially_copyable_v<bupp::async_io::datagram_socket_view>);
  static_assert(
      std::is_standard_layout_v<bupp::async_io::datagram_socket_view>);
  static_assert(can_bind<bupp::async_io::stream_socket_view,
                         const bupp::async_io::ip::endpoint&>);
  static_assert(can_connect<bupp::async_io::stream_socket_view,
                            const bupp::async_io::ip::endpoint&>);
  static_assert(can_listen<bupp::async_io::stream_socket_view, int>);
  static_assert(can_bind<bupp::async_io::datagram_socket_view,
                         const bupp::async_io::ip::endpoint&>);
  static_assert(can_connect<bupp::async_io::datagram_socket_view,
                            const bupp::async_io::ip::endpoint&>);
  static_assert(!can_bind<bupp::async_io::stream_socket_view, const sockaddr*,
                          socklen_t>);
  static_assert(!can_connect<bupp::async_io::stream_socket_view,
                             const sockaddr*, socklen_t>);
  static_assert(!can_listen<bupp::async_io::datagram_socket_view, int>);
  static_assert(!has_sync_datagram_io<bupp::async_io::datagram_socket_view>);
}
