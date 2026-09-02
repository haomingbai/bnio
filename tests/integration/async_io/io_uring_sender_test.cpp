#include <bnio/async_io/ip/endpoint.h>
#include <bnio/async_io/linux/io_uring_operations/socket.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <system_error>

#include "../../support/async_io/io_uring_context_test_support.h"

namespace {

using namespace bnio_async_io_io_uring_test;

TEST(IoUringSenderTest, post_operation_runs_on_context_thread) {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  receiver recv;
  recv.context = &context;
  recv.stop_on_completion = true;
  auto state = recv.state;

  io_uring_post_operation operation(context, std::move(recv));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_TRUE(state->in_context);
}

TEST(IoUringSenderTest, poll_sender_observes_pipe_readiness) {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  int descriptors[2] = {-1, -1};
  EXPECT_EQ(::pipe2(descriptors, O_CLOEXEC), 0);

  poll_receiver recv;
  recv.context = &context;
  auto state = recv.state;

  auto sender = context.async_poll(descriptor_view(descriptors[0]),
                                   static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(recv));
  bexec::start(operation);

  constexpr char byte = 'x';
  EXPECT_EQ(::write(descriptors[1], &byte, sizeof(byte)),
            static_cast<ssize_t>(sizeof(byte)));
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_TRUE((static_cast<unsigned>(state->result) &
               static_cast<unsigned>(POLLIN)) != 0);
  EXPECT_TRUE(state->in_context);

  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

TEST(IoUringSenderTest, poll_sender_reports_bad_descriptor) {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  poll_receiver recv;
  recv.context = &context;
  auto state = recv.state;

  auto sender =
      context.async_poll(descriptor_view(-1), static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(recv));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::error);
  EXPECT_EQ(state->error, std::error_code(EBADF, std::generic_category()));
  EXPECT_TRUE(state->in_context);
}

TEST(IoUringSenderTest, resolve_sender_runs_on_context_thread) {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  bnio::async_io::dns_query query("127.0.0.1", "8080");
  query.set_address_version(bnio::async_io::ip::address::version::v4);
  std::array<bnio::async_io::ip::endpoint, 8> results{};

  resolve_receiver recv;
  resolve_state state;
  recv.state = &state;
  recv.context = &context;

  auto sender = context.async_resolve(std::move(query),
                                      bnio::async_io::dns_result_view(results));
  auto operation = bexec::connect(std::move(sender), std::move(recv));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state.signal, signal_kind::value);
  EXPECT_GT(state.endpoint_count, 0);
  EXPECT_EQ(results[0].port(), 8080);
  EXPECT_EQ(results[0].address().type(),
            bnio::async_io::ip::address::version::v4);
  EXPECT_TRUE(state.in_context);
}

TEST(IoUringSenderTest, nop_operation_completes_with_raw_cqe) {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  receiver recv;
  recv.context = &context;
  recv.stop_on_completion = true;
  auto state = recv.state;

  io_uring_nop_operation operation(context, std::move(recv));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->result, 0);
  EXPECT_TRUE(state->in_context);
}

TEST(IoUringSenderTest, stop_token_completes_stopped_before_submit) {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  bexec::inplace_stop_source source;
  EXPECT_TRUE(source.request_stop());

  stopped_receiver recv;
  recv.context = &context;
  recv.env = stop_env{source.get_token()};
  recv.stop_on_completion = true;
  auto state = recv.state;

  io_uring_nop_operation operation(context, std::move(recv));
  bexec::start(operation);
  context.run();

  // User stop-token cancellation completes through set_stopped().
  EXPECT_EQ(state->signal, signal_kind::stopped);
  EXPECT_TRUE(state->in_context);
}

TEST(IoUringSenderTest, accept_cancel_preserves_remote_endpoint) {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  const int listener_fd =
      ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
  EXPECT_TRUE(listener_fd >= 0);

  // 1.2.3.4 in host byte order.
  const bnio::async_io::ip::endpoint initial_endpoint(
      bnio::async_io::ip::address::v4(0x01020304), 5);
  bnio::async_io::ip::endpoint remote_endpoint = initial_endpoint;

  bexec::inplace_stop_source source;
  EXPECT_TRUE(source.request_stop());

  stopped_receiver recv;
  recv.context = &context;
  recv.env = stop_env{source.get_token()};
  recv.stop_on_completion = true;
  auto state = recv.state;

  io_uring_accept_operation operation(context, stream_socket_view(listener_fd),
                                      remote_endpoint, 0, std::move(recv));
  bexec::start(operation);
  context.run();

  // User stop-token cancellation completes through set_stopped().
  EXPECT_EQ(state->signal, signal_kind::stopped);
  EXPECT_TRUE(remote_endpoint.address().is_v4());
  EXPECT_EQ(remote_endpoint.address().to_v4(),
            initial_endpoint.address().to_v4());
  EXPECT_EQ(remote_endpoint.port(), initial_endpoint.port());

  EXPECT_EQ(::close(listener_fd), 0);
}

TEST(IoUringSenderTest, receive_from_cancel_preserves_remote_endpoint) {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  const int receiver_fd =
      ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
  EXPECT_TRUE(receiver_fd >= 0);

  // 1.2.3.4 in host byte order.
  const bnio::async_io::ip::endpoint initial_endpoint(
      bnio::async_io::ip::address::v4(0x01020304), 5);
  bnio::async_io::ip::endpoint remote_endpoint = initial_endpoint;

  std::array<char, 32> bytes{};

  bexec::inplace_stop_source source;
  EXPECT_TRUE(source.request_stop());

  stopped_receiver recv;
  recv.context = &context;
  recv.env = stop_env{source.get_token()};
  recv.stop_on_completion = true;
  auto state = recv.state;

  io_uring_receive_from_operation operation(
      context, datagram_socket_view(receiver_fd),
      buffer_view(bytes.data(), bytes.size()), remote_endpoint, 0,
      std::move(recv));
  bexec::start(operation);
  context.run();

  // User stop-token cancellation completes through set_stopped().
  EXPECT_EQ(state->signal, signal_kind::stopped);
  EXPECT_TRUE(remote_endpoint.address().is_v4());
  EXPECT_EQ(remote_endpoint.address().to_v4(),
            initial_endpoint.address().to_v4());
  EXPECT_EQ(remote_endpoint.port(), initial_endpoint.port());

  EXPECT_EQ(::close(receiver_fd), 0);
}

}  // namespace
