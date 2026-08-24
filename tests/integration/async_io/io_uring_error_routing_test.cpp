// Regression tests for io_uring_context error-routing defects. Red
// phase: every test below FAILS on the unfixed code for its intended
// reason and must pass once errors route through
// run_phase::finish_drain (or queue_exit delivery during teardown).
//
// Covered defects:
//   - wait_for_io_work() pre-block re-arm failure exits the phase
//     machine straight to finished, skipping finish(): inflight ops
//     are stranded with no terminal receiver call.
//   - enter_run() re-arm failure makes run() return while ops posted
//     to the shared queues are stranded.
//   - queue_exit() aborts inflight/shared-io ops but then discards
//     the aborted completions (pop_cpu_all), so their receivers are
//     never notified.
//   - a fatal error from wait_for_cqe_event() (io_uring_enter returning
//     e.g. -EBADF/-EINVAL rather than -ETIME/-EINTR) is documented
//     below (no deterministic public-API trigger).
//
// Fatal wait error note (documentation only, no runtime test):
//
//   A fatal error from wait_for_cqe_event() hits the same broken
//   routing as the pre-block re-arm failure: wait_for_io_work() used to
//   store finished and return run_phase::finished, skipping finish()
//   drain/abort. There is no deterministic way to make a healthy ring's
//   io_uring_enter fail via the public API alone (the ring fd is
//   private to the context and valid for the whole run), so this gap is
//   documented here instead of being exercised by a test. Any fix must
//   route the wait_for_cqe_event() fatal branch through
//   run_phase::finish_drain as well. The second pre-block re-arm site
//   (the per-worker local wake channel) is covered by
//   local_wake_channel_close_in_context_strands_inflight_io.

#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <memory>
#include <utility>
#include <vector>

#include "../../support/async_io/io_uring_context_test_support.h"

namespace {

using namespace bnio_async_io_io_uring_test;

using bnio::async_io::linux_native::io_uring_poll_sender_operation;

// Receiver for inflight poll operations that only records the terminal
// signal. Unlike the shared poll_receiver, it never calls stop(), so the
// error-routing paths under test stay deterministic.
struct terminal_poll_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  io_uring_context* context = nullptr;

  void set_value(std::error_code ec, unsigned events) noexcept {
    if (ec) {
      state->signal = signal_kind::error;
      state->error = ec;
    } else {
      state->signal = signal_kind::value;
      state->result = static_cast<int>(events);
    }
    state->in_context = (context != nullptr && context->is_in_context());
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    state->in_context = (context != nullptr && context->is_in_context());
  }
};

// Which wake channel a channel_closing_post_receiver closes in-context.
enum class close_target {
  // Shared io_context-wide wake channel (submit_eventfd_poll site).
  shared_channel,
  // Per-worker local wake channel (submit_local_eventfd_poll site).
  local_channel,
};

// Posted-task receiver that closes a wake channel while executing
// in-context, simulating a wake-channel failure observed mid-run.
struct channel_closing_post_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  io_uring_context* context = nullptr;
  close_target target = close_target::shared_channel;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      state->signal = signal_kind::error;
      state->error = ec;
    } else {
      state->signal = signal_kind::value;
    }
    state->in_context = (context != nullptr && context->is_in_context());
    if (context != nullptr) {
      if (target == close_target::shared_channel) {
        context->get_global_state()->wake_channel_.close();
      } else {
        context->local_state()->wake_channel_.close();
      }
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    state->in_context = (context != nullptr && context->is_in_context());
  }
};

// Asserts that an operation reached a terminal receiver call: never
// signal_kind::none. Returns true when the signal is terminal.
::testing::AssertionResult reached_terminal(
    const char* name, signal_kind signal, const char* defect) {
  if (signal == signal_kind::none) {
    return ::testing::AssertionFailure()
           << defect << ": operation \"" << name
           << "\" never received a terminal receiver call "
              "(set_value with error or set_stopped)";
  }
  return ::testing::AssertionSuccess();
}

// wait_for_io_work() re-arms the shared eventfd poll before blocking.
// When the shared wake channel is closed while a poll operation is
// inflight, submit_eventfd_poll() fails (-EINVAL: read_fd() is -1) and
// the run loop jumps straight to run_phase::finished, skipping finish():
// the inflight operation is stranded with no terminal receiver call even
// though run() returns.
TEST(IoUringErrorRoutingTest,
     global_wake_channel_close_in_context_strands_inflight_io) {
  io_uring_task_queue_state global_tasks;
  // Operation storage is declared before the context so the operations
  // outlive ~io_uring_context(): its queue_exit()->abort_inflight_io()
  // must never touch freed operations.
  std::unique_ptr<io_uring_poll_sender_operation<terminal_poll_receiver>>
      poll_op;
  std::unique_ptr<io_uring_post_operation<channel_closing_post_receiver>>
      post_op;
  io_uring_context context;
  if (!queue_init_shared_or_skip(context, global_tasks)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::pipe2(descriptors, O_CLOEXEC), 0);

  // (a) Poll on a pipe read end that never becomes readable: after the
  // run loop submits it, the operation stays inflight with no CQE.
  terminal_poll_receiver poll_recv;
  poll_recv.context = &context;
  auto poll_state = poll_recv.state;
  poll_op =
      std::make_unique<io_uring_poll_sender_operation<terminal_poll_receiver>>(
          context, descriptor_view(descriptors[0]),
          static_cast<unsigned>(POLLIN), std::move(poll_recv));
  bexec::start(*poll_op);

  // (b) Posted task whose receiver closes the shared wake channel while
  // executing in-context. The run loop executes the CPU task first and
  // submits the poll operation in the next run_ready_tasks pass, so the
  // channel is already closed when wait_for_io_work() re-arms.
  channel_closing_post_receiver post_recv;
  post_recv.context = &context;
  post_recv.target = close_target::shared_channel;
  auto post_state = post_recv.state;
  post_op = std::make_unique<
      io_uring_post_operation<channel_closing_post_receiver>>(
      context, std::move(post_recv));
  bexec::start(*post_op);

  // (c) run() on the test thread. Pre-fix it returns through the broken
  // re-arm path; post-fix it must return through the finish drain.
  bool run_completed = false;
  context.run();
  run_completed = true;
  EXPECT_TRUE(run_completed) << "run() must return";

  // The posted task must have executed normally before the failure.
  EXPECT_EQ(post_state->signal, signal_kind::value)
      << "posted task was stranded before the re-arm failure path was "
         "reached";

  // The inflight poll operation must reach a terminal signal.
  EXPECT_TRUE(reached_terminal("poll", poll_state->signal, "shared-wake"));
  EXPECT_TRUE(poll_state->signal == signal_kind::stopped ||
              poll_state->signal == signal_kind::error)
      << "shared-wake: expected aborted inflight poll to complete with "
         "set_stopped or set_value(error)";

  if (descriptors[0] >= 0) {
    (void)::close(descriptors[0]);
  }
  if (descriptors[1] >= 0) {
    (void)::close(descriptors[1]);
  }
}

// Sibling branch: same scenario, but the posted receiver closes the
// per-worker LOCAL wake channel so the failure comes from
// submit_local_eventfd_poll() (the second pre-block re-arm site in
// wait_for_io_work()). The shared-channel re-arm still succeeds (its poll
// is already pending), so only the local re-arm fails.
TEST(IoUringErrorRoutingTest,
     local_wake_channel_close_in_context_strands_inflight_io) {
  io_uring_task_queue_state global_tasks;
  std::unique_ptr<io_uring_poll_sender_operation<terminal_poll_receiver>>
      poll_op;
  std::unique_ptr<io_uring_post_operation<channel_closing_post_receiver>>
      post_op;
  io_uring_context context;
  if (!queue_init_shared_or_skip(context, global_tasks)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::pipe2(descriptors, O_CLOEXEC), 0);

  terminal_poll_receiver poll_recv;
  poll_recv.context = &context;
  auto poll_state = poll_recv.state;
  poll_op =
      std::make_unique<io_uring_poll_sender_operation<terminal_poll_receiver>>(
          context, descriptor_view(descriptors[0]),
          static_cast<unsigned>(POLLIN), std::move(poll_recv));
  bexec::start(*poll_op);

  channel_closing_post_receiver post_recv;
  post_recv.context = &context;
  post_recv.target = close_target::local_channel;
  auto post_state = post_recv.state;
  post_op = std::make_unique<
      io_uring_post_operation<channel_closing_post_receiver>>(
      context, std::move(post_recv));
  bexec::start(*post_op);

  bool run_completed = false;
  context.run();
  run_completed = true;
  EXPECT_TRUE(run_completed) << "run() must return";

  EXPECT_EQ(post_state->signal, signal_kind::value)
      << "posted task was stranded before the re-arm failure path was "
         "reached";

  EXPECT_TRUE(reached_terminal("poll", poll_state->signal, "shared-wake"));
  EXPECT_TRUE(poll_state->signal == signal_kind::stopped ||
              poll_state->signal == signal_kind::error)
      << "shared-wake: expected aborted inflight poll to complete with "
         "set_stopped or set_value(error)";

  if (descriptors[0] >= 0) {
    (void)::close(descriptors[0]);
  }
  if (descriptors[1] >= 0) {
    (void)::close(descriptors[1]);
  }
}

// When the shared wake channel is closed BEFORE run(),
// enter_run()'s submit_eventfd_poll() fails and run() returns false-side
// immediately, stranding every operation already posted to the shared
// CPU queue and published to the shared I/O queue.
TEST(IoUringErrorRoutingTest,
     enter_run_failure_strands_posted_and_published_ops) {
  io_uring_task_queue_state global_tasks;
  std::vector<std::shared_ptr<shared_state>> post_states;
  std::vector<std::unique_ptr<io_uring_post_operation<receiver>>> post_ops;
  std::unique_ptr<io_uring_poll_sender_operation<terminal_poll_receiver>>
      poll_op;
  io_uring_context context;
  if (!queue_init_shared_or_skip(context, global_tasks)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::pipe2(descriptors, O_CLOEXEC), 0);

  constexpr unsigned k_posts = 3;
  for (unsigned index = 0; index < k_posts; ++index) {
    receiver recv;
    recv.context = &context;
    recv.stop_on_completion = false;
    post_states.push_back(recv.state);
    post_ops.push_back(
        std::make_unique<io_uring_post_operation<receiver>>(
            context, std::move(recv)));
    bexec::start(*post_ops.back());
  }

  terminal_poll_receiver poll_recv;
  poll_recv.context = &context;
  auto poll_state = poll_recv.state;
  poll_op =
      std::make_unique<io_uring_poll_sender_operation<terminal_poll_receiver>>(
          context, descriptor_view(descriptors[0]),
          static_cast<unsigned>(POLLIN), std::move(poll_recv));
  bexec::start(*poll_op);

  // Close the shared wake channel externally, before run().
  global_tasks.wake_channel_.close();

  bool run_completed = false;
  context.run();
  run_completed = true;
  EXPECT_TRUE(run_completed) << "run() must return";

  for (unsigned index = 0; index < k_posts; ++index) {
    EXPECT_TRUE(reached_terminal("post", post_states[index]->signal, "enter-run"))
        << "posted task " << index << " stranded";
  }
  EXPECT_TRUE(reached_terminal("poll", poll_state->signal, "enter-run"));
  EXPECT_TRUE(poll_state->signal == signal_kind::stopped ||
              poll_state->signal == signal_kind::error)
      << "enter-run: expected published poll op to complete with "
         "set_stopped or set_value(error)";

  if (descriptors[0] >= 0) {
    (void)::close(descriptors[0]);
  }
  if (descriptors[1] >= 0) {
    (void)::close(descriptors[1]);
  }
}

// queue_exit() aborts ops found inflight or in the shared I/O queue
// (marking them -ECANCELED + complete_submit_stopped) and pushes them
// onto the local CPU queue, but the very next pop_cpu_all() discards
// them: the receivers never run. Destroying a context (or calling
// queue_exit()) with published-but-never-run I/O must still deliver a
// terminal signal, synchronously on the calling thread.
TEST(IoUringErrorRoutingTest, queue_exit_discards_aborted_io_completions) {
  io_uring_task_queue_state global_tasks;
  std::unique_ptr<io_uring_poll_sender_operation<terminal_poll_receiver>>
      poll_op;
  io_uring_context context;
  if (!queue_init_shared_or_skip(context, global_tasks)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::pipe2(descriptors, O_CLOEXEC), 0);

  // Publish an I/O operation from the test thread; no worker ever runs,
  // so the operation sits in the shared I/O queue until queue_exit().
  terminal_poll_receiver poll_recv;
  poll_recv.context = &context;
  auto poll_state = poll_recv.state;
  poll_op =
      std::make_unique<io_uring_poll_sender_operation<terminal_poll_receiver>>(
          context, descriptor_view(descriptors[0]),
          static_cast<unsigned>(POLLIN), std::move(poll_recv));
  bexec::start(*poll_op);

  context.queue_exit();

  EXPECT_TRUE(reached_terminal("poll", poll_state->signal, "queue-exit"));
  EXPECT_TRUE(poll_state->signal == signal_kind::stopped ||
              poll_state->signal == signal_kind::error)
      << "queue-exit: expected aborted poll op to be delivered with set_stopped "
         "or set_value(error) before queue_exit() returned";

  if (descriptors[0] >= 0) {
    (void)::close(descriptors[0]);
  }
  if (descriptors[1] >= 0) {
    (void)::close(descriptors[1]);
  }
}

}  // namespace
