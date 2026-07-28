#include <bnio/async_io/dns/resolve.h>
#include <bnio/async_io/dns/types.h>
#include <bnio/async_io/ip/address.h>
#include <bnio/async_io/ip/endpoint.h>
#include <bnio/async_io/socket_view.h>
#include <bnio/buffer/basic.h>
#include <bnio/buffer/dynamic_string.h>
#include <bnio/buffer/holders.h>
#include <bnio/io_context.h>
#include <bnio/ip.h>
#include <bnio/udp.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "../../support/io_context/io_context_runtime_test_support.h"

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
  EXPECT_TRUE(flags >= 0);
  EXPECT_TRUE((flags & FD_CLOEXEC) != 0);
}

template <class Scheduler, class Socket, class Buffer>
concept scheduler_can_send_datagram =
    requires(Scheduler scheduler, Socket socket, Buffer buffer) {
      scheduler.async_send(socket, buffer);
    };

template <class Scheduler, class Socket, class Buffer>
concept scheduler_can_receive_datagram =
    requires(Scheduler scheduler, Socket socket, Buffer buffer) {
      scheduler.async_receive(socket, buffer);
    };

template <class Socket>
concept has_sync_datagram_io = requires(
    Socket socket, bnio::const_buffer output, bnio::mutable_buffer input,
    bnio::ip::endpoint endpoint, std::error_code error) {
  socket.send(output, 0, error);
  socket.receive(input, 0, error);
  socket.send_to(output, endpoint, 0, error);
  socket.receive_from(input, endpoint, 0, error);
};

struct pair_state {
  unsigned completions = 0;
  std::size_t sent = 0;
  std::size_t received = 0;
  std::error_code error;
};

struct send_receiver {
  pair_state* state;
  bnio::io_context* context;

  void set_value(std::size_t size) noexcept {
    state->sent = size;
    complete();
  }
  void set_error(std::error_code error) noexcept {
    state->error = error;
    complete();
  }
  void set_stopped() noexcept { complete(); }

 private:
  void complete() noexcept {
    if (++state->completions == 2) {
      (void)context->stop();
    }
  }
};

struct receive_receiver {
  pair_state* state;
  bnio::io_context* context;

  void set_value(std::size_t size) noexcept {
    state->received = size;
    complete();
  }
  void set_error(std::error_code error) noexcept {
    state->error = error;
    complete();
  }
  void set_stopped() noexcept { complete(); }

 private:
  void complete() noexcept {
    if (++state->completions == 2) {
      (void)context->stop();
    }
  }
};

TEST(UdpTest, sender_concepts) {
  bnio::io_context context;
  auto scheduler = context.get_post_scheduler();
  bnio::udp::socket socket;
  std::array<char, 8> bytes{};
  const std::string_view payload = "udp";
  bnio::ip::endpoint endpoint;

  using send_sender = decltype(socket.async_send(scheduler, payload));
  using receive_sender = decltype(socket.async_receive(scheduler, bytes));
  using send_to_sender =
      decltype(socket.async_send_to(scheduler, payload, endpoint));
  using receive_from_sender =
      decltype(socket.async_receive_from(scheduler, bytes, endpoint));
  using low_send_sender =
      decltype(scheduler.async_send(socket.view(), bnio::buffer(payload)));
  using low_receive_sender =
      decltype(scheduler.async_receive(socket.view(), bnio::buffer(bytes)));

  static_assert(bexec::sender<send_sender>);
  static_assert(bexec::sender<receive_sender>);
  static_assert(bexec::sender<send_to_sender>);
  static_assert(bexec::sender<receive_from_sender>);
  static_assert(bexec::sender<low_send_sender>);
  static_assert(bexec::sender<low_receive_sender>);
  static_assert(
      scheduler_can_send_datagram<decltype(scheduler), decltype(socket.view()),
                                  bnio::const_buffer>);
  static_assert(
      scheduler_can_receive_datagram<
          decltype(scheduler), decltype(socket.view()), bnio::mutable_buffer>);
  static_assert(!scheduler_can_send_datagram<decltype(scheduler),
                                             bnio::async_io::stream_socket_view,
                                             bnio::const_buffer>);
  static_assert(
      !scheduler_can_receive_datagram<decltype(scheduler),
                                      bnio::async_io::stream_socket_view,
                                      bnio::mutable_buffer>);
  static_assert(
      !scheduler_can_write_stream<decltype(scheduler),
                                  bnio::async_io::datagram_socket_view,
                                  bnio::const_buffer>);
  static_assert(!scheduler_can_read_stream<decltype(scheduler),
                                           bnio::async_io::datagram_socket_view,
                                           bnio::mutable_buffer>);
}

TEST(UdpTest, protocol_and_lifecycle) {
  static_assert(std::is_same_v<bnio::ip::udp::socket, bnio::udp::socket>);
  static_assert(std::is_same_v<bnio::udp_socket, bnio::udp::socket>);
  static_assert(!std::is_copy_constructible_v<bnio::udp::socket>);
  static_assert(std::is_nothrow_move_constructible_v<bnio::udp::socket>);
  static_assert(!has_sync_datagram_io<bnio::udp::socket>);

  EXPECT_EQ(bnio::ip::udp::v4().version(), bnio::ip::address::version::v4);
  EXPECT_EQ(bnio::ip::udp::v6().version(), bnio::ip::address::version::v6);

  auto query =
      bnio::udp::make_resolve_query("127.0.0.1", 443, bnio::ip::udp::v4());
  EXPECT_EQ(query.transport(), bnio::dns_transport::udp);
  EXPECT_EQ(query.address_version(), bnio::ip::address::version::v4);
  EXPECT_EQ(query.service(), "443");
  std::array<bnio::ip::endpoint, 4> resolved{};
  std::size_t resolved_count = 0;
  EXPECT_FALSE(bnio::async_io::resolve_dns(
      query, bnio::dns_result_view(resolved), resolved_count));
  EXPECT_GT(resolved_count, 0);
  EXPECT_TRUE(resolved[0].address().is_v4());
  EXPECT_EQ(resolved[0].port(), 443);

  bnio::udp::socket unspecified;
  const std::error_code error = unspecified.open(bnio::ip::udp());
  EXPECT_EQ(error, std::error_code(EAFNOSUPPORT, std::generic_category()));

  bnio::udp::socket first;
  EXPECT_FALSE(first.open(bnio::ip::udp::v4()));
  const int fd = first.native_handle();
  EXPECT_TRUE(fd >= 0);
  bnio::udp::socket second(std::move(first));
  EXPECT_FALSE(first.is_open());
  EXPECT_EQ(second.native_handle(), fd);
  EXPECT_EQ(second.release(), fd);
  EXPECT_EQ(::close(fd), 0);
}

TEST(UdpTest, socket_ownership_and_error_paths) {
  bnio::udp::socket socket;
  EXPECT_FALSE(socket.close());
  EXPECT_FALSE(socket.open(bnio::ip::udp::v4()));
  const int transferred_fd = socket.native_handle();
  assert_close_on_exec(transferred_fd);
  EXPECT_FALSE(socket.open(bnio::ip::udp::v4()));
  EXPECT_EQ(socket.native_handle(), transferred_fd);
  self_move_assign(socket);
  EXPECT_EQ(socket.native_handle(), transferred_fd);

  bnio::udp::socket destination;
  EXPECT_FALSE(destination.open(bnio::ip::udp::v4()));
  const int replaced_fd = destination.native_handle();
  destination = std::move(socket);
  EXPECT_FALSE(socket.is_open());
  EXPECT_EQ(destination.native_handle(), transferred_fd);
  assert_descriptor_closed(replaced_fd);

  bnio::udp::socket replacement;
  EXPECT_FALSE(replacement.open(bnio::ip::udp::v4()));
  const int replacement_fd = replacement.release();
  destination.assign(replacement_fd);
  EXPECT_EQ(destination.native_handle(), replacement_fd);
  assert_descriptor_closed(transferred_fd);
  destination.assign(replacement_fd);
  EXPECT_TRUE(::fcntl(replacement_fd, F_GETFD) >= 0);
  EXPECT_FALSE(destination.set_reuse_address(true));
  EXPECT_FALSE(destination.set_reuse_address(false));
  EXPECT_FALSE(destination.close());
  assert_descriptor_closed(replacement_fd);
  EXPECT_FALSE(destination.close());

  bnio::udp::socket invalid_family;
  EXPECT_TRUE(invalid_family.open(AF_UNSPEC));
  EXPECT_FALSE(invalid_family.is_open());
}

TEST(UdpTest, socket_destructor_and_invalid_descriptor_errors) {
  int owned_fd = -1;
  {
    bnio::udp::socket owned;
    EXPECT_FALSE(owned.open(bnio::ip::udp::v4()));
    owned_fd = owned.native_handle();
  }
  assert_descriptor_closed(owned_fd);

  bnio::udp::socket externally_closed;
  EXPECT_FALSE(externally_closed.open(bnio::ip::udp::v4()));
  const int fd = externally_closed.native_handle();
  EXPECT_EQ(::close(fd), 0);
  const std::error_code close_error = externally_closed.close();
  EXPECT_EQ(close_error, std::error_code(EBADF, std::generic_category()));
  EXPECT_FALSE(externally_closed.is_open());

  bnio::udp::socket closed;
  EXPECT_EQ(closed.shutdown(SHUT_RDWR),
            std::error_code(EBADF, std::generic_category()));
  EXPECT_EQ(closed.set_reuse_address(true),
            std::error_code(EBADF, std::generic_category()));

  bnio::ip::endpoint endpoint = bnio::ip::endpoint::loopback_v4(1234);
  EXPECT_EQ(closed.local_endpoint(endpoint),
            std::error_code(EBADF, std::generic_category()));
  EXPECT_EQ(endpoint.version(), bnio::ip::address::version::unspecified);
  endpoint = bnio::ip::endpoint::loopback_v4(1234);
  EXPECT_EQ(closed.remote_endpoint(endpoint),
            std::error_code(EBADF, std::generic_category()));
  EXPECT_EQ(endpoint.version(), bnio::ip::address::version::unspecified);
}

TEST(UdpTest, async_send_to_receive_from) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  bnio::udp::socket server;
  bnio::udp::socket client;
  EXPECT_FALSE(server.open(bnio::ip::udp::v4()));
  EXPECT_FALSE(client.open(bnio::ip::udp::v4()));
  EXPECT_FALSE(server.bind(bnio::ip::endpoint::loopback_v4(0)));
  bnio::ip::endpoint server_endpoint;
  EXPECT_FALSE(server.local_endpoint(server_endpoint));

  constexpr std::string_view payload = "asynchronous datagram";
  std::string received;
  bnio::ip::endpoint source;
  pair_state state;

  auto receive_sender = server.async_receive_from(
      scheduler, bnio::dynamic_buffer(received), source);
  auto send_sender = client.async_send_to(scheduler, payload, server_endpoint);

  auto receive_operation = bexec::connect(std::move(receive_sender),
                                          receive_receiver{&state, &context});
  auto send_operation =
      bexec::connect(std::move(send_sender), send_receiver{&state, &context});
  bexec::start(receive_operation);
  bexec::start(send_operation);

  context.run();

  EXPECT_FALSE(state.error);
  EXPECT_EQ(state.completions, 2);
  EXPECT_EQ(state.sent, payload.size());
  EXPECT_EQ(state.received, payload.size());
  EXPECT_EQ(received, payload);
  EXPECT_TRUE(source.address().is_v4());
  EXPECT_EQ(source.address().to_v4(), bnio::ip::address::loopback_v4().to_v4());
  EXPECT_NE(source.port(), 0);
}

TEST(UdpTest, async_default_peer) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  bnio::udp::socket server;
  bnio::udp::socket client;
  EXPECT_FALSE(server.open(bnio::ip::udp::v4()));
  EXPECT_FALSE(client.open(bnio::ip::udp::v4()));
  EXPECT_FALSE(server.bind(bnio::ip::endpoint::loopback_v4(0)));
  EXPECT_FALSE(client.bind(bnio::ip::endpoint::loopback_v4(0)));

  bnio::ip::endpoint server_endpoint;
  bnio::ip::endpoint client_endpoint;
  EXPECT_FALSE(server.local_endpoint(server_endpoint));
  EXPECT_FALSE(client.local_endpoint(client_endpoint));
  EXPECT_FALSE(server.connect(client_endpoint));
  EXPECT_FALSE(client.connect(server_endpoint));

  constexpr std::string_view payload = "default-peer";
  std::string received;
  pair_state state;
  auto receive_sender =
      server.async_receive(scheduler, bnio::dynamic_buffer(received));
  auto send_sender = client.async_send(scheduler, payload);
  auto receive_operation = bexec::connect(std::move(receive_sender),
                                          receive_receiver{&state, &context});
  auto send_operation =
      bexec::connect(std::move(send_sender), send_receiver{&state, &context});
  bexec::start(receive_operation);
  bexec::start(send_operation);

  context.run();

  EXPECT_FALSE(state.error);
  EXPECT_EQ(state.completions, 2);
  EXPECT_EQ(state.sent, payload.size());
  EXPECT_EQ(state.received, payload.size());
  EXPECT_EQ(received, payload);
}

TEST(UdpTest, async_ipv6_send_to) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  bnio::udp::socket server;
  bnio::udp::socket client;
  const std::error_code server_open = server.open(bnio::ip::udp::v6());
  const std::error_code client_open = client.open(bnio::ip::udp::v6());
  if (server_open == std::errc::address_family_not_supported ||
      client_open == std::errc::address_family_not_supported) {
    GTEST_SKIP() << "IPv6 not supported";
  }
  EXPECT_FALSE(server_open);
  EXPECT_FALSE(client_open);
  const std::error_code bind_error =
      server.bind(bnio::ip::endpoint::loopback_v6(0));
  if (bind_error == std::errc::address_not_available) {
    GTEST_SKIP() << "address not available";
  }
  EXPECT_FALSE(bind_error);

  bnio::ip::endpoint destination;
  EXPECT_FALSE(server.local_endpoint(destination));
  constexpr std::string_view payload = "ipv6";
  std::string received;
  bnio::ip::endpoint source;
  pair_state state;
  auto receive_operation =
      bexec::connect(server.async_receive_from(
                         scheduler, bnio::dynamic_buffer(received), source),
                     receive_receiver{&state, &context});
  auto send_operation =
      bexec::connect(client.async_send_to(scheduler, payload, destination),
                     send_receiver{&state, &context});
  bexec::start(receive_operation);
  bexec::start(send_operation);
  context.run();

  EXPECT_FALSE(state.error);
  EXPECT_EQ(state.sent, payload.size());
  EXPECT_EQ(state.received, payload.size());
  EXPECT_EQ(received, payload);
  EXPECT_TRUE(source.address().is_v6());
}

}  // namespace
