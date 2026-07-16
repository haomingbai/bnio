#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <system_error>

#include "kqueue_context_test_support.h"

namespace {

using namespace bupp_async_io_kqueue_test;

TEST(KqueueSenderTest, post_operation_runs_on_context_thread) {
  kqueue_context context;
  EXPECT_TRUE(context.queue_init() == 0);

  receiver completion;
  completion.context = &context;
  completion.stop_on_completion = true;
  auto state = completion.state;

  kqueue_post_operation operation(context, std::move(completion));
  bexec::start(operation);
  context.run();

  EXPECT_TRUE(state->signal == signal_kind::value);
  EXPECT_TRUE(state->in_context);
}

TEST(KqueueSenderTest, nop_operation_completes_on_context_thread) {
  kqueue_context context;
  EXPECT_TRUE(context.queue_init() == 0);

  receiver completion;
  completion.context = &context;
  completion.stop_on_completion = true;
  auto state = completion.state;

  kqueue_nop_operation operation(context, std::move(completion));
  bexec::start(operation);
  context.run();

  EXPECT_TRUE(state->signal == signal_kind::value);
  EXPECT_TRUE(state->result == 0);
  EXPECT_TRUE(state->in_context);
}

TEST(KqueueSenderTest, io_preparation_is_deferred_to_the_run_loop) {
  kqueue_context context;
  EXPECT_TRUE(context.queue_init() == 0);

  receiver completion;
  completion.context = &context;
  completion.stop_on_completion = true;
  auto state = completion.state;
  bool prepared = false;

  auto prepare = [&context, &prepared](kqueue_helper& helper) noexcept {
    prepared = context.is_in_context();
    helper.prep_nop();
  };
  kqueue_raw_operation operation(context, std::move(prepare),
                                 std::move(completion));
  bexec::start(operation);

  EXPECT_TRUE(!prepared);
  context.run();

  EXPECT_TRUE(prepared);
  EXPECT_TRUE(state->signal == signal_kind::value);
  EXPECT_TRUE(state->in_context);
}

TEST(KqueueSenderTest, poll_sender_observes_pipe_readiness) {
  kqueue_context context;
  EXPECT_TRUE(context.queue_init() == 0);

  int descriptors[2] = {-1, -1};
  EXPECT_TRUE(::pipe(descriptors) == 0);

  poll_receiver completion;
  completion.context = &context;
  auto state = completion.state;
  auto sender = context.async_poll(descriptor_view(descriptors[0]),
                                   static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(completion));
  bexec::start(operation);

  constexpr char byte = 'x';
  EXPECT_TRUE(::write(descriptors[1], &byte, 1) == 1);
  context.run();

  EXPECT_TRUE(state->signal == signal_kind::value);
  EXPECT_TRUE((static_cast<unsigned>(state->result) &
               static_cast<unsigned>(POLLIN)) != 0);
  EXPECT_TRUE(state->in_context);
  EXPECT_TRUE(::close(descriptors[0]) == 0);
  EXPECT_TRUE(::close(descriptors[1]) == 0);
}

TEST(KqueueSenderTest, poll_sender_reports_bad_descriptor) {
  kqueue_context context;
  EXPECT_TRUE(context.queue_init() == 0);

  poll_receiver completion;
  completion.context = &context;
  auto state = completion.state;
  auto sender =
      context.async_poll(descriptor_view(-1), static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(completion));
  bexec::start(operation);
  context.run();

  EXPECT_TRUE(state->signal == signal_kind::error);
  EXPECT_TRUE(state->error == std::error_code(EBADF, std::generic_category()));
  EXPECT_TRUE(state->in_context);
}

TEST(KqueueSenderTest, poll_sender_accepts_read_and_write_filters) {
  kqueue_context context;
  EXPECT_TRUE(context.queue_init() == 0);

  int descriptors[2] = {-1, -1};
  EXPECT_TRUE(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
  constexpr char byte = 'x';
  EXPECT_TRUE(::write(descriptors[1], &byte, 1) == 1);

  poll_receiver completion;
  completion.context = &context;
  auto state = completion.state;
  auto sender = context.async_poll(descriptor_view(descriptors[0]),
                                   static_cast<unsigned>(POLLIN | POLLOUT));
  auto operation = bexec::connect(std::move(sender), std::move(completion));
  bexec::start(operation);
  context.run();

  EXPECT_TRUE(state->signal == signal_kind::value);
  const unsigned ready = static_cast<unsigned>(state->result);
  EXPECT_TRUE((ready & static_cast<unsigned>(POLLIN | POLLOUT)) != 0);
  EXPECT_TRUE(::close(descriptors[0]) == 0);
  EXPECT_TRUE(::close(descriptors[1]) == 0);
}

TEST(KqueueSenderTest, context_performs_bounded_pipe_read_and_write) {
  kqueue_context context;
  EXPECT_TRUE(context.queue_init() == 0);

  int descriptors[2] = {-1, -1};
  EXPECT_TRUE(::pipe(descriptors) == 0);
  std::array<char, 32> input{};
  std::array<char, 32> output{};
  constexpr char message[] = "kqueue-buffer-view";
  std::memcpy(input.data(), message, sizeof(message));

  receiver write_completion;
  write_completion.context = &context;
  auto write_state = write_completion.state;
  kqueue_write_operation write_operation(
      context, descriptor_view(descriptors[1]),
      buffer_view{input.data(), sizeof(message)}, std::move(write_completion));

  receiver read_completion;
  read_completion.context = &context;
  read_completion.stop_on_completion = true;
  auto read_state = read_completion.state;
  kqueue_read_operation read_operation(
      context, descriptor_view(descriptors[0]),
      buffer_view{output.data(), output.size()}, std::move(read_completion));

  bexec::start(read_operation);
  bexec::start(write_operation);
  context.run();

  EXPECT_TRUE(write_state->signal == signal_kind::value);
  EXPECT_TRUE(write_state->result == static_cast<int>(sizeof(message)));
  EXPECT_TRUE(read_state->signal == signal_kind::value);
  EXPECT_TRUE(read_state->result == static_cast<int>(sizeof(message)));
  EXPECT_TRUE(std::memcmp(input.data(), output.data(), sizeof(message)) == 0);
  EXPECT_TRUE(write_state->in_context);
  EXPECT_TRUE(read_state->in_context);
  EXPECT_TRUE(::close(descriptors[0]) == 0);
  EXPECT_TRUE(::close(descriptors[1]) == 0);
}

TEST(KqueueSenderTest, read_reports_clean_pipe_eof) {
  kqueue_context context;
  EXPECT_TRUE(context.queue_init() == 0);

  int descriptors[2] = {-1, -1};
  EXPECT_TRUE(::pipe(descriptors) == 0);
  EXPECT_TRUE(::close(descriptors[1]) == 0);
  std::array<char, 8> output{};

  receiver completion;
  completion.context = &context;
  completion.stop_on_completion = true;
  auto state = completion.state;
  kqueue_read_operation operation(context, descriptor_view(descriptors[0]),
                                  buffer_view{output.data(), output.size()},
                                  std::move(completion));
  bexec::start(operation);
  context.run();

  EXPECT_TRUE(state->signal == signal_kind::value);
  EXPECT_TRUE(state->result == 0);
  EXPECT_TRUE(::close(descriptors[0]) == 0);
}

TEST(KqueueSenderTest, read_never_exceeds_buffer_view_size) {
  kqueue_context context;
  EXPECT_TRUE(context.queue_init() == 0);

  int descriptors[2] = {-1, -1};
  EXPECT_TRUE(::pipe(descriptors) == 0);
  std::array<char, 64> input{};
  std::array<char, 8> output{};
  input.fill('z');
  EXPECT_TRUE(::write(descriptors[1], input.data(), input.size()) ==
              static_cast<ssize_t>(input.size()));

  receiver completion;
  completion.context = &context;
  completion.stop_on_completion = true;
  auto state = completion.state;
  kqueue_read_operation operation(context, descriptor_view(descriptors[0]),
                                  buffer_view{output.data(), output.size()},
                                  std::move(completion));
  bexec::start(operation);
  context.run();

  EXPECT_TRUE(state->signal == signal_kind::value);
  EXPECT_TRUE(state->result == static_cast<int>(output.size()));
  for (char byte : output) {
    EXPECT_TRUE(byte == 'z');
  }
  EXPECT_TRUE(::close(descriptors[0]) == 0);
  EXPECT_TRUE(::close(descriptors[1]) == 0);
}

TEST(KqueueSenderTest, duplicate_filter_waiters_are_queued) {
  kqueue_context context;
  EXPECT_TRUE(context.queue_init() == 0);

  int descriptors[2] = {-1, -1};
  EXPECT_TRUE(::pipe(descriptors) == 0);
  std::array<char, 1> first_buffer{};
  std::array<char, 1> second_buffer{};

  receiver first_completion;
  first_completion.context = &context;
  auto first_state = first_completion.state;
  kqueue_read_operation first_operation(
      context, descriptor_view(descriptors[0]),
      buffer_view{first_buffer.data(), first_buffer.size()},
      std::move(first_completion));

  receiver second_completion;
  second_completion.context = &context;
  second_completion.stop_on_completion = true;
  auto second_state = second_completion.state;
  kqueue_read_operation second_operation(
      context, descriptor_view(descriptors[0]),
      buffer_view{second_buffer.data(), second_buffer.size()},
      std::move(second_completion));

  bexec::start(first_operation);
  bexec::start(second_operation);
  constexpr std::array<char, 2> input{'a', 'b'};
  EXPECT_TRUE(::write(descriptors[1], input.data(), input.size()) ==
              static_cast<ssize_t>(input.size()));
  context.run();

  EXPECT_TRUE(first_state->signal == signal_kind::value);
  EXPECT_TRUE(first_state->result == 1);
  EXPECT_TRUE(second_state->signal == signal_kind::value);
  EXPECT_TRUE(second_state->result == 1);
  EXPECT_TRUE(first_buffer[0] == input[0]);
  EXPECT_TRUE(second_buffer[0] == input[1]);
  EXPECT_TRUE(::close(descriptors[0]) == 0);
  EXPECT_TRUE(::close(descriptors[1]) == 0);
}

TEST(KqueueSenderTest, stop_token_completes_before_native_registration) {
  kqueue_context context;
  EXPECT_TRUE(context.queue_init() == 0);

  bexec::inplace_stop_source source;
  EXPECT_TRUE(source.request_stop());

  stopped_receiver completion;
  completion.context = &context;
  completion.stop_on_completion = true;
  completion.environment = stop_env{source.get_token()};
  auto state = completion.state;
  kqueue_nop_operation operation(context, std::move(completion));
  bexec::start(operation);
  context.run();

  EXPECT_TRUE(state->signal == signal_kind::stopped);
  EXPECT_TRUE(state->in_context);
}

}  // namespace
