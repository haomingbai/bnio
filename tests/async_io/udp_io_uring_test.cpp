#include <bupp/async_io/buffer_view.h>
#include <bupp/async_io/ip/address.h>
#include <bupp/async_io/ip/endpoint.h>
#include <bupp/async_io/linux/io_uring_context_base/context.h>
#include <bupp/async_io/linux/io_uring_operations/socket.h>
#include <bupp/async_io/socket_view.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <bexec/operation_state.hpp>
#include <cstring>
#include <string_view>
#include <system_error>

#include "io_uring_context_test_support.h"

namespace {

using namespace bupp_async_io_io_uring_test;

struct transfer_state {
  unsigned completions = 0;
  int sent = -1;
  int received = -1;
  std::error_code error;
};

struct transfer_receiver {
  transfer_state* state;
  io_uring_context* context;
  bool receive;

  void set_value(int result, unsigned) noexcept {
    if (receive) {
      state->received = result;
    } else {
      state->sent = result;
    }
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

TEST(UdpIoUringTest, send_to_receive_from_operations) {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  const int receiver_fd =
      ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
  const int sender_fd =
      ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
  EXPECT_TRUE(receiver_fd >= 0);
  EXPECT_TRUE(sender_fd >= 0);

  datagram_socket_view receiver_socket(receiver_fd);
  datagram_socket_view sender_socket(sender_fd);
  EXPECT_TRUE(
      !receiver_socket.bind(bupp::async_io::ip::endpoint::loopback_v4(0)));
  bupp::async_io::ip::endpoint receiver_endpoint;
  EXPECT_TRUE(!receiver_socket.local_endpoint(receiver_endpoint));

  constexpr std::string_view payload = "low-level UDP";
  std::array<char, 32> bytes{};
  bupp::async_io::ip::endpoint source;
  transfer_state state;

  io_uring_receive_from_operation receive_operation(
      context, receiver_socket, buffer_view(bytes.data(), bytes.size()), source,
      0, transfer_receiver{&state, &context, true});
  io_uring_send_to_operation send_operation(
      context, sender_socket,
      buffer_view(const_cast<char*>(payload.data()), payload.size()),
      receiver_endpoint, 0, transfer_receiver{&state, &context, false});

  bexec::start(receive_operation);
  bexec::start(send_operation);
  context.run();

  EXPECT_TRUE(!state.error);
  EXPECT_TRUE(state.completions == 2);
  EXPECT_TRUE(state.sent == static_cast<int>(payload.size()));
  EXPECT_TRUE(state.received == static_cast<int>(payload.size()));
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
  EXPECT_TRUE(source.address().is_v4());
  EXPECT_TRUE(source.port() != 0);

  EXPECT_TRUE(::close(receiver_fd) == 0);
  EXPECT_TRUE(::close(sender_fd) == 0);
}

TEST(UdpIoUringTest, connected_datagram_operations) {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  const int receiver_fd =
      ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
  const int sender_fd =
      ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
  EXPECT_TRUE(receiver_fd >= 0);
  EXPECT_TRUE(sender_fd >= 0);

  datagram_socket_view receiver_socket(receiver_fd);
  datagram_socket_view sender_socket(sender_fd);
  EXPECT_TRUE(
      !receiver_socket.bind(bupp::async_io::ip::endpoint::loopback_v4(0)));
  EXPECT_TRUE(
      !sender_socket.bind(bupp::async_io::ip::endpoint::loopback_v4(0)));

  bupp::async_io::ip::endpoint receiver_endpoint;
  bupp::async_io::ip::endpoint sender_endpoint;
  EXPECT_TRUE(!receiver_socket.local_endpoint(receiver_endpoint));
  EXPECT_TRUE(!sender_socket.local_endpoint(sender_endpoint));
  EXPECT_TRUE(!receiver_socket.connect(sender_endpoint));
  EXPECT_TRUE(!sender_socket.connect(receiver_endpoint));

  constexpr std::string_view payload = "connected low-level UDP";
  std::array<char, 32> bytes{};
  transfer_state state;

  io_uring_datagram_receive_operation receive_operation(
      context, receiver_socket, buffer_view(bytes.data(), bytes.size()), 0,
      transfer_receiver{&state, &context, true});
  io_uring_datagram_send_operation send_operation(
      context, sender_socket,
      buffer_view(const_cast<char*>(payload.data()), payload.size()), 0,
      transfer_receiver{&state, &context, false});

  bexec::start(receive_operation);
  bexec::start(send_operation);
  context.run();

  EXPECT_TRUE(!state.error);
  EXPECT_TRUE(state.completions == 2);
  EXPECT_TRUE(state.sent == static_cast<int>(payload.size()));
  EXPECT_TRUE(state.received == static_cast<int>(payload.size()));
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);

  EXPECT_TRUE(::close(receiver_fd) == 0);
  EXPECT_TRUE(::close(sender_fd) == 0);
}

}  // namespace
