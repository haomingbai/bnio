#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <system_error>

#include "kqueue_context_test_support.h"

namespace {

using namespace bupp_async_io_kqueue_test;

void test_post_operation_runs_on_context_thread() {
  kqueue_context context;
  assert(context.queue_init() == 0);

  receiver completion;
  completion.context = &context;
  completion.stop_on_completion = true;
  auto state = completion.state;

  kqueue_post_operation operation(context, std::move(completion));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->in_context);
}

void test_nop_operation_completes_on_context_thread() {
  kqueue_context context;
  assert(context.queue_init() == 0);

  receiver completion;
  completion.context = &context;
  completion.stop_on_completion = true;
  auto state = completion.state;

  kqueue_nop_operation operation(context, std::move(completion));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->result == 0);
  assert(state->in_context);
}

void test_poll_sender_observes_pipe_readiness() {
  kqueue_context context;
  assert(context.queue_init() == 0);

  int descriptors[2] = {-1, -1};
  assert(::pipe(descriptors) == 0);

  poll_receiver completion;
  completion.context = &context;
  auto state = completion.state;
  auto sender = context.async_poll(descriptor_view(descriptors[0]),
                                   static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(completion));
  bexec::start(operation);

  constexpr char byte = 'x';
  assert(::write(descriptors[1], &byte, 1) == 1);
  context.run();

  assert(state->signal == signal_kind::value);
  assert((static_cast<unsigned>(state->result) &
          static_cast<unsigned>(POLLIN)) != 0);
  assert(state->in_context);
  assert(::close(descriptors[0]) == 0);
  assert(::close(descriptors[1]) == 0);
}

void test_poll_sender_reports_bad_descriptor() {
  kqueue_context context;
  assert(context.queue_init() == 0);

  poll_receiver completion;
  completion.context = &context;
  auto state = completion.state;
  auto sender =
      context.async_poll(descriptor_view(-1), static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(completion));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::error);
  assert(state->error == std::error_code(EBADF, std::generic_category()));
  assert(state->in_context);
}

void test_poll_sender_accepts_read_and_write_filters() {
  kqueue_context context;
  assert(context.queue_init() == 0);

  int descriptors[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
  constexpr char byte = 'x';
  assert(::write(descriptors[1], &byte, 1) == 1);

  poll_receiver completion;
  completion.context = &context;
  auto state = completion.state;
  auto sender = context.async_poll(descriptor_view(descriptors[0]),
                                   static_cast<unsigned>(POLLIN | POLLOUT));
  auto operation = bexec::connect(std::move(sender), std::move(completion));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::value);
  const unsigned ready = static_cast<unsigned>(state->result);
  assert((ready & static_cast<unsigned>(POLLIN | POLLOUT)) != 0);
  assert(::close(descriptors[0]) == 0);
  assert(::close(descriptors[1]) == 0);
}

void test_context_performs_bounded_pipe_read_and_write() {
  kqueue_context context;
  assert(context.queue_init() == 0);

  int descriptors[2] = {-1, -1};
  assert(::pipe(descriptors) == 0);
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

  assert(write_state->signal == signal_kind::value);
  assert(write_state->result == static_cast<int>(sizeof(message)));
  assert(read_state->signal == signal_kind::value);
  assert(read_state->result == static_cast<int>(sizeof(message)));
  assert(std::memcmp(input.data(), output.data(), sizeof(message)) == 0);
  assert(write_state->in_context);
  assert(read_state->in_context);
  assert(::close(descriptors[0]) == 0);
  assert(::close(descriptors[1]) == 0);
}

void test_read_reports_clean_pipe_eof() {
  kqueue_context context;
  assert(context.queue_init() == 0);

  int descriptors[2] = {-1, -1};
  assert(::pipe(descriptors) == 0);
  assert(::close(descriptors[1]) == 0);
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

  assert(state->signal == signal_kind::value);
  assert(state->result == 0);
  assert(::close(descriptors[0]) == 0);
}

void test_read_never_exceeds_buffer_view_size() {
  kqueue_context context;
  assert(context.queue_init() == 0);

  int descriptors[2] = {-1, -1};
  assert(::pipe(descriptors) == 0);
  std::array<char, 64> input{};
  std::array<char, 8> output{};
  input.fill('z');
  assert(::write(descriptors[1], input.data(), input.size()) ==
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

  assert(state->signal == signal_kind::value);
  assert(state->result == static_cast<int>(output.size()));
  for (char byte : output) {
    assert(byte == 'z');
  }
  assert(::close(descriptors[0]) == 0);
  assert(::close(descriptors[1]) == 0);
}

void test_duplicate_filter_waiter_reports_busy() {
  kqueue_context context;
  assert(context.queue_init() == 0);

  int descriptors[2] = {-1, -1};
  assert(::pipe(descriptors) == 0);
  std::array<char, 1> first_buffer{};
  std::array<char, 1> second_buffer{};

  receiver first_completion;
  first_completion.context = &context;
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
  context.run();

  assert(second_state->signal == signal_kind::error);
  assert(second_state->error ==
         std::error_code(EBUSY, std::generic_category()));
  assert(::close(descriptors[0]) == 0);
  assert(::close(descriptors[1]) == 0);
}

void test_stop_token_completes_before_native_registration() {
  kqueue_context context;
  assert(context.queue_init() == 0);

  bexec::inplace_stop_source source;
  assert(source.request_stop());

  stopped_receiver completion;
  completion.context = &context;
  completion.stop_on_completion = true;
  completion.environment = stop_env{source.get_token()};
  auto state = completion.state;
  kqueue_nop_operation operation(context, std::move(completion));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::stopped);
  assert(state->in_context);
}

}  // namespace

int main() {
  test_post_operation_runs_on_context_thread();
  test_nop_operation_completes_on_context_thread();
  test_poll_sender_observes_pipe_readiness();
  test_poll_sender_reports_bad_descriptor();
  test_poll_sender_accepts_read_and_write_filters();
  test_context_performs_bounded_pipe_read_and_write();
  test_read_reports_clean_pipe_eof();
  test_read_never_exceeds_buffer_view_size();
  test_duplicate_filter_waiter_reports_busy();
  test_stop_token_completes_before_native_registration();
  return 0;
}
