#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <system_error>
#include <thread>

#include "../../support/async_io/kqueue_context_test_support.h"

namespace {

using namespace bnio_async_io_kqueue_test;

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate&& predicate,
                              std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

TEST(KqueueSenderTest, post_operation_runs_on_context_thread) {
  kqueue_context context;
  EXPECT_EQ(context.queue_init(), 0);

  receiver completion;
  completion.context = &context;
  completion.stop_on_completion = true;
  auto state = completion.state;

  kqueue_post_operation operation(context, std::move(completion));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_TRUE(state->in_context);
}

TEST(KqueueSenderTest, nop_operation_completes_on_context_thread) {
  kqueue_context context;
  EXPECT_EQ(context.queue_init(), 0);

  receiver completion;
  completion.context = &context;
  completion.stop_on_completion = true;
  auto state = completion.state;

  kqueue_nop_operation operation(context, std::move(completion));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->result, 0);
  EXPECT_TRUE(state->in_context);
}

TEST(KqueueSenderTest, io_preparation_is_deferred_to_the_run_loop) {
  kqueue_context context;
  EXPECT_EQ(context.queue_init(), 0);

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

  EXPECT_FALSE(prepared);
  context.run();

  EXPECT_TRUE(prepared);
  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_TRUE(state->in_context);
}

TEST(KqueueSenderTest, poll_sender_observes_pipe_readiness) {
  kqueue_context context;
  EXPECT_EQ(context.queue_init(), 0);

  int descriptors[2] = {-1, -1};
  EXPECT_EQ(::pipe(descriptors), 0);

  poll_receiver completion;
  completion.context = &context;
  auto state = completion.state;
  auto sender = context.async_poll(descriptor_view(descriptors[0]),
                                   static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(completion));
  bexec::start(operation);

  constexpr char byte = 'x';
  EXPECT_EQ(::write(descriptors[1], &byte, 1), 1);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_TRUE((static_cast<unsigned>(state->result) &
               static_cast<unsigned>(POLLIN)) != 0);
  EXPECT_TRUE(state->in_context);
  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

TEST(KqueueSenderTest, poll_sender_reports_bad_descriptor) {
  kqueue_context context;
  EXPECT_EQ(context.queue_init(), 0);

  poll_receiver completion;
  completion.context = &context;
  auto state = completion.state;
  auto sender =
      context.async_poll(descriptor_view(-1), static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(completion));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::error);
  EXPECT_EQ(state->error, std::error_code(EBADF, std::generic_category()));
  // The errno travels in ec; the ready mask must be 0, never
  // static_cast<unsigned>(-EBADF).
  EXPECT_EQ(state->poll_events, 0u);
  EXPECT_TRUE(state->in_context);
}

// A stop token already cancelled at start() is observed by start(), which marks
// the completion stopped; execute() then delivers set_stopped. No mask is
// delivered on the stop channel at all, so the recorded payload stays 0.
TEST(KqueueSenderTest, poll_sender_cancelled_before_start_stops) {
  kqueue_context context;
  EXPECT_EQ(context.queue_init(), 0);

  int descriptors[2] = {-1, -1};
  EXPECT_EQ(::pipe(descriptors), 0);

  bexec::inplace_stop_source source;
  EXPECT_TRUE(source.request_stop());

  stopped_poll_receiver completion;
  completion.context = &context;
  completion.environment = stop_env{source.get_token()};
  auto state = completion.state;
  auto sender = context.async_poll(descriptor_view(descriptors[0]),
                                   static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(completion));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::stopped);
  EXPECT_EQ(state->poll_events, 0u);

  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

TEST(KqueueSenderTest, poll_sender_accepts_read_and_write_filters) {
  kqueue_context context;
  EXPECT_EQ(context.queue_init(), 0);

  int descriptors[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors), 0);
  constexpr char byte = 'x';
  EXPECT_EQ(::write(descriptors[1], &byte, 1), 1);

  poll_receiver completion;
  completion.context = &context;
  auto state = completion.state;
  auto sender = context.async_poll(descriptor_view(descriptors[0]),
                                   static_cast<unsigned>(POLLIN | POLLOUT));
  auto operation = bexec::connect(std::move(sender), std::move(completion));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  const unsigned ready = static_cast<unsigned>(state->result);
  EXPECT_TRUE((ready & static_cast<unsigned>(POLLIN | POLLOUT)) != 0);
  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

TEST(KqueueSenderTest, context_performs_bounded_pipe_read_and_write) {
  kqueue_context context;
  EXPECT_EQ(context.queue_init(), 0);

  int descriptors[2] = {-1, -1};
  EXPECT_EQ(::pipe(descriptors), 0);
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

  EXPECT_EQ(write_state->signal, signal_kind::value);
  EXPECT_EQ(write_state->result, static_cast<int>(sizeof(message)));
  EXPECT_EQ(read_state->signal, signal_kind::value);
  EXPECT_EQ(read_state->result, static_cast<int>(sizeof(message)));
  EXPECT_TRUE(std::memcmp(input.data(), output.data(), sizeof(message)) == 0);
  EXPECT_TRUE(write_state->in_context);
  EXPECT_TRUE(read_state->in_context);
  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

TEST(KqueueSenderTest, read_reports_clean_pipe_eof) {
  kqueue_context context;
  EXPECT_EQ(context.queue_init(), 0);

  int descriptors[2] = {-1, -1};
  EXPECT_EQ(::pipe(descriptors), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
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

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->result, 0);
  EXPECT_EQ(::close(descriptors[0]), 0);
}

TEST(KqueueSenderTest, read_never_exceeds_buffer_view_size) {
  kqueue_context context;
  EXPECT_EQ(context.queue_init(), 0);

  int descriptors[2] = {-1, -1};
  EXPECT_EQ(::pipe(descriptors), 0);
  std::array<char, 64> input{};
  std::array<char, 8> output{};
  input.fill('z');
  EXPECT_EQ(::write(descriptors[1], input.data(), input.size()),
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

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->result, static_cast<int>(output.size()));
  for (char byte : output) {
    EXPECT_EQ(byte, 'z');
  }
  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

TEST(KqueueSenderTest, duplicate_filter_waiters_are_queued) {
  kqueue_context context;
  EXPECT_EQ(context.queue_init(), 0);

  int descriptors[2] = {-1, -1};
  EXPECT_EQ(::pipe(descriptors), 0);
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
  EXPECT_EQ(::write(descriptors[1], input.data(), input.size()),
            static_cast<ssize_t>(input.size()));
  context.run();

  EXPECT_EQ(first_state->signal, signal_kind::value);
  EXPECT_EQ(first_state->result, 1);
  EXPECT_EQ(second_state->signal, signal_kind::value);
  EXPECT_EQ(second_state->result, 1);
  EXPECT_EQ(first_buffer[0], input[0]);
  EXPECT_EQ(second_buffer[0], input[1]);
  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

TEST(KqueueSenderTest, stop_token_completes_before_native_registration) {
  kqueue_context context;
  EXPECT_EQ(context.queue_init(), 0);

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

  // User stop-token cancellation completes through set_stopped().
  EXPECT_EQ(state->signal, signal_kind::stopped);
  EXPECT_TRUE(state->in_context);
}

// queue_exit() on a context whose run() was never called: an operation
// published into the shared I/O queue (publish_io takes the shared path
// off the run-loop thread) is never registered, so its only chance at a
// terminal call is the teardown drain.  queue_exit() must deliver it
// through the stop channel — set_value(operation_canceled) for this
// token-less receiver — synchronously on the calling thread, never
// silently drop it.  This shape is kqueue-specific: the native
// publish_io is void with no life_state check (the io_uring publish
// path rejects once stopping), so a publication racing the teardown has
// no inline-rejection fallback and relies on the drain entirely.
TEST(KqueueSenderTest, queue_exit_delivers_published_but_unrun_io) {
  kqueue_task_queue_state global_tasks;
  ASSERT_GE(global_tasks.wake_channel_.open(), 0);
  kqueue_context context;
  context.set_global_state(&global_tasks);
  ASSERT_EQ(context.queue_init(), 0);

  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::pipe(descriptors), 0);

  poll_receiver completion;
  completion.context = &context;
  auto state = completion.state;
  auto sender = context.async_poll(descriptor_view(descriptors[0]),
                                   static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(completion));
  // The calling thread is outside run(): publish_io routes to the shared
  // I/O queue, and run() is deliberately never called, so nobody drains
  // it before the teardown.
  bexec::start(operation);
  context.queue_exit();

  EXPECT_EQ(state->signal, signal_kind::error);
  EXPECT_EQ(state->error, std::make_error_code(std::errc::operation_canceled));
  // The abort carries no ready mask; the errno rides in ec.
  EXPECT_EQ(state->poll_events, 0u);
  // Delivered synchronously on the calling thread, not on a worker.
  EXPECT_FALSE(state->in_context);

  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

// stop() racing an in-flight poll: the worker parked on the poll, the
// caller publishes the closing state (io_context::begin_stop()'s
// life_state = 1) and stops the native context; the wake write unblocks
// the kevent wait and finish()'s abort path delivers the in-flight poll
// through the stop channel — set_value(operation_canceled) for this
// token-less receiver — executed on the worker thread.  Together with
// queue_exit_delivers_published_but_unrun_io this pins the kqueue-side
// abort/error-code contract that used to be asserted only against
// io_uring.
TEST(KqueueSenderTest, stop_aborts_inflight_poll_reports_operation_canceled) {
  kqueue_task_queue_state global_tasks;
  ASSERT_GE(global_tasks.wake_channel_.open(), 0);
  kqueue_context context;
  context.set_global_state(&global_tasks);
  ASSERT_EQ(context.queue_init(), 0);

  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::pipe(descriptors), 0);

  poll_receiver completion;
  completion.context = &context;
  auto state = completion.state;
  auto sender = context.async_poll(descriptor_view(descriptors[0]),
                                   static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(completion));
  bexec::start(operation);  // shared queue; the worker registers it

  std::thread worker([&context] { context.run(); });
  ASSERT_TRUE(wait_until([&] { return context.is_waiting(); },
                         std::chrono::milliseconds(2000)))
      << "worker never parked with the poll in flight";

  // io_context::stop() semantics: publish the closing state, then stop
  // the native context (finishing + wake write).
  global_tasks.life_state.store(1, std::memory_order_release);
  (void)context.stop();
  worker.join();

  EXPECT_EQ(state->signal, signal_kind::error);
  EXPECT_EQ(state->error, std::make_error_code(std::errc::operation_canceled));
  EXPECT_EQ(state->poll_events, 0u);
  EXPECT_TRUE(state->in_context);  // delivered on the worker thread

  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

// EAGAIN re-arm contract at its source (kqueue_context::perform_io_step,
// src/async_io/bsd/kqueue_context_events.cpp:94).  With two read waiters
// queued on one pipe read end, process_event() arms the successor
// *before* the fired head performs its read — and the byte is still in
// the pipe at that moment, so the kernel reports the successor ready.
// The head's read then consumes the byte, the successor's perform_io()
// hits -EAGAIN, and try_rearm_operation() re-arms it to stay in flight.
// Nothing may deliver -EAGAIN to any receiver: the second waiter must
// stay silent until new data arrives, then complete normally with
// exactly one byte.
TEST(KqueueSenderTest, rearm_keeps_second_waiter_pending_until_data_arrives) {
  kqueue_context context;
  ASSERT_EQ(context.queue_init(), 0);

  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::pipe(descriptors), 0);
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

  // The pipe is empty here: both waiters probe -EAGAIN on their first
  // non-blocking read and publish to the I/O queue.
  bexec::start(first_operation);
  bexec::start(second_operation);

  std::thread worker([&context] { context.run(); });
  ASSERT_TRUE(wait_until([&] { return context.is_waiting(); },
                         std::chrono::milliseconds(2000)))
      << "worker never parked with both waiters registered";

  // One byte fires the armed head; arming the successor races the head's
  // read by construction (see process_event()).
  EXPECT_EQ(::write(descriptors[1], "a", 1), 1);
  ASSERT_TRUE(
      wait_until([&] { return first_state->signal == signal_kind::value; },
                 std::chrono::milliseconds(2000)))
      << "first waiter never completed";
  EXPECT_EQ(first_state->result, 1);
  EXPECT_EQ(first_buffer[0], 'a');

  // The successor was armed while the byte was still pending, so its
  // ready event raced the head's read: perform_io() hit -EAGAIN and the
  // re-arm kept it in flight.  Give the run loop a bounded settle window,
  // then require zero terminal calls on the second waiter.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(second_state->signal, signal_kind::none)
      << "second waiter completed or errored instead of staying pending "
         "after the raced -EAGAIN";

  // New data completes the re-armed waiter normally.
  EXPECT_EQ(::write(descriptors[1], "b", 1), 1);
  ASSERT_TRUE(
      wait_until([&] { return second_state->signal == signal_kind::value; },
                 std::chrono::milliseconds(2000)))
      << "re-armed second waiter never completed after new data";
  EXPECT_EQ(second_state->result, 1);
  EXPECT_EQ(second_buffer[0], 'b');
  // Zero -EAGAIN deliveries anywhere: both waiters end on the value
  // channel (an EAGAIN error would have latched signal_kind::error).
  EXPECT_EQ(first_state->signal, signal_kind::value);
  EXPECT_EQ(second_state->signal, signal_kind::value);

  worker.join();

  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

}  // namespace
