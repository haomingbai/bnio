#include <gtest/gtest.h>

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

  EXPECT_EQ(state->signal, signal_kind::stopped);
  EXPECT_TRUE(state->in_context);
}

}  // namespace
