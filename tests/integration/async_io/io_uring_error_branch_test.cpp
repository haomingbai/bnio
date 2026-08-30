// Branch-coverage tests for the io_uring_context error-routing hardening
// and the neighbouring poll-state, stop-signal, timer-deadline,
// queue_exit-delivery, and CQE-dispatch-tier branches.
//
// Every test uses only the public context API plus the shared harness
// (io_uring_task_queue_state's public fields included); no private state
// is touched. Compared with io_uring_error_routing_test.cpp these tests
// target branch outcomes rather than defect regressions:
//   - enter_run() local-channel re-arm failure;
//   - stop() signal error paths (-EBADF after channel close, idempotent
//     re-stop);
//   - graceful stop with inflight I/O: re-arm returns "not armed"
//     (0) and the worker parks unarmed waiting for inflight CQEs
//     (wait_for_io_work's poll_result==0 && !should_finish() outcome);
//   - -EINTR spurious wakeup propagation (never fatal);
//   - timer-heap deadline wait (-ETIME expiry) and empty-heap park;
//   - local directed wake (wake_one_sleeping) CQE collection;
//   - queue_exit() delivery of an operation published from a stopped
//     receiver (abort_and_deliver_completions' consume loop);
//   - CQE dispatch tiers 2/3 (local budget / shared-queue spill);
//   - timer-heap fetch returning true without a deadline
//     (time_point::max() -> unbounded park, wake polls as the source);
//   - queue_init() validation branches (entries==0, flags==0 retry,
//     double init, zero CQE window).

#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "../../support/async_io/io_uring_context_test_support.h"

namespace {

using namespace bnio_async_io_io_uring_test;

using bnio::async_io::clock;
using bnio::async_io::linux_native::io_uring_io_operation_base;
using bnio::async_io::linux_native::io_uring_poll_sender_operation;
using bnio::async_io::time_point;

// Bounded wait for every synchronization point: a healthy run loop reacts
// within milliseconds; only a regression can exceed this bound.
constexpr auto kSyncBound = std::chrono::seconds(5);

// Polls predicate() every 200us until it holds or the bound elapses.
template <typename Predicate>
bool wait_until(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + kSyncBound;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return true;
}

// Receiver for inflight poll operations that only records the terminal
// signal; it never calls stop() so the paths under test stay
// deterministic. Identical to the one in io_uring_error_routing_test.cpp.
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

// When the per-worker LOCAL wake channel is closed before
// run(), enter_run()'s local eventfd arm_wake_poll() fails and run() must
// still deliver every operation already posted to the shared CPU queue
// and published to the shared I/O queue through finish()'s abort path.
TEST(IoUringErrorBranchTest, enter_run_local_channel_close_delivers_ops) {
  io_uring_task_queue_state global_tasks;
  std::unique_ptr<io_uring_poll_sender_operation<terminal_poll_receiver>>
      poll_op;
  std::vector<std::unique_ptr<io_uring_post_operation<receiver>>> post_ops;
  io_uring_context context;
  if (!queue_init_shared_or_skip(context, global_tasks)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::pipe2(descriptors, O_CLOEXEC), 0);

  constexpr unsigned k_posts = 2;
  std::vector<std::shared_ptr<shared_state>> post_states;
  for (unsigned index = 0; index < k_posts; ++index) {
    receiver recv;
    recv.context = &context;
    recv.stop_on_completion = false;
    post_states.push_back(recv.state);
    post_ops.push_back(std::make_unique<io_uring_post_operation<receiver>>(
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

  // Close the per-worker channel so enter_run()'s local re-arm fails
  // (-EINVAL: the channel read fd is gone) after the shared re-arm
  // succeeded.
  context.local_state()->wake_channel_.close();

  context.run();

  for (unsigned index = 0; index < k_posts; ++index) {
    EXPECT_EQ(post_states[index]->signal, signal_kind::value)
        << "posted task " << index << " stranded by the enter_run failure";
  }
  EXPECT_TRUE(poll_state->signal == signal_kind::stopped ||
              poll_state->signal == signal_kind::error)
      << "published poll op stranded by the enter_run failure";
  EXPECT_TRUE(poll_state->in_context)
      << "aborted completion must be delivered on the run()-caller thread";

  if (descriptors[0] >= 0) {
    (void)::close(descriptors[0]);
  }
  if (descriptors[1] >= 0) {
    (void)::close(descriptors[1]);
  }
}

// stop() error/idempotency branches: signalling twice while finishing,
// signalling after the shared wake channel closed (-EBADF), and stopping
// an already finished context (returns 0 without signalling).
TEST(IoUringErrorBranchTest, stop_signal_error_paths_and_idempotency) {
  io_uring_task_queue_state global_tasks;
  io_uring_context context;
  if (!queue_init_shared_or_skip(context, global_tasks)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  // First stop: running -> finishing, wake succeeds (0).
  EXPECT_EQ(context.stop(), 0);
  // Second stop while already finishing: falls through to signal again.
  EXPECT_EQ(context.stop(), 0);

  // Signal path with the shared wake channel closed.
  global_tasks.wake_channel_.close();
  EXPECT_EQ(context.stop(), -EBADF);

  // Stop after teardown: the state is finished, so stop() is a no-op.
  context.queue_exit();
  EXPECT_EQ(context.stop(), 0);
}

// Graceful stop with inflight I/O: after stop() the pre-block re-arm
// reports "not armed" (0) because the context is stopping; with the
// inflight poll still pending, should_finish() is false and the worker
// must park in io_uring_enter with no armed wake poll, relying on the
// inflight CQE as its only wake source (grace-wait). Completing the
// inflight I/O then finishes the run loop.
TEST(IoUringErrorBranchTest, graceful_stop_parks_unarmed_for_inflight) {
  io_uring_task_queue_state global_tasks;
  std::unique_ptr<io_uring_poll_sender_operation<terminal_poll_receiver>>
      poll_op;
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

  std::atomic<bool> run_done{false};
  std::thread worker([&context, &run_done] {
    context.run();
    run_done.store(true, std::memory_order_release);
  });

  // Park #1: the poll op is inflight (consumed before the worker can
  // park), the wake polls are armed.
  ASSERT_TRUE(wait_until([&context] { return context.is_waiting(); }))
      << "worker never parked with the inflight poll";

  context.stop();

  // The stop wake ends park #1; with the op still inflight the worker
  // passes the unarmed re-arm and parks again (grace-wait) within
  // microseconds — far faster than is_waiting() could be observed
  // false. Just give that pass time to run before producing the CQE
  // the worker is grace-waiting for.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  ASSERT_EQ(::write(descriptors[1], "x", 1), 1);

  ASSERT_TRUE(wait_until([&run_done] { return run_done.load(); }))
      << "worker never finished after the inflight completion";
  worker.join();

  EXPECT_EQ(poll_state->signal, signal_kind::value);
  EXPECT_EQ(poll_state->result, POLLIN);
  EXPECT_TRUE(poll_state->in_context);

  if (descriptors[0] >= 0) {
    (void)::close(descriptors[0]);
  }
  if (descriptors[1] >= 0) {
    (void)::close(descriptors[1]);
  }
}

// Installs a no-op SIGUSR1 handler (without SA_RESTART) exactly once.
void install_noop_sigusr1_handler() {
  static std::once_flag once;
  std::call_once(once, [] {
    struct sigaction action;
    action.sa_handler = [](int) {};
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    (void)::sigaction(SIGUSR1, &action, nullptr);
  });
}

// A signal delivered to the parked run-loop thread makes
// io_uring_enter return -EINTR. The run loop must treat it as a
// spurious wakeup (never a fatal ring error), re-evaluate its state,
// and park again until stop() finishes it.
TEST(IoUringErrorBranchTest, spurious_signal_does_not_stop_run_loop) {
  io_uring_task_queue_state global_tasks;
  io_uring_context context;
  if (!queue_init_shared_or_skip(context, global_tasks)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }
  install_noop_sigusr1_handler();

  pthread_t worker_tid{};
  std::atomic<bool> run_done{false};
  std::thread worker([&context, &run_done, &worker_tid] {
    worker_tid = pthread_self();
    context.run();
    run_done.store(true, std::memory_order_release);
  });

  ASSERT_TRUE(wait_until([&context] { return context.is_waiting(); }))
      << "worker never parked";

  // Deliver the signal while parked so io_uring_enter returns -EINTR;
  // give the worker a moment to take the spurious-wakeup path and park
  // again before finishing it.
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  ASSERT_EQ(::pthread_kill(worker_tid, SIGUSR1), 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));

  context.stop();

  ASSERT_TRUE(wait_until([&run_done] { return run_done.load(); }))
      << "run loop treated -EINTR as fatal or never finished";
  worker.join();
}

// Stub for the shared lazy timer heap: reports a deadline once armed and
// hands out one due operation after the deadline passes. Call-site
// agnostic, so the exact spin/wait call sequence does not matter.
struct timer_stub_state {
  bool armed = false;
  bool delivered = false;
  bool always_empty = false;
  // Never reports a deadline: every fetch answers true with
  // time_point::max() (an armed heap with no timer to wait for).
  bool no_deadline = false;
  clock::time_point fire_at{};
  io_uring_operation_base* due_op = nullptr;
};

bool timer_stub_fetch(void* heap, time_point& deadline,
                      io_uring_operation_base*& ops) noexcept {
  auto* state = static_cast<timer_stub_state*>(heap);
  if (state->always_empty) {
    return false;
  }
  if (state->no_deadline) {
    deadline = time_point::max();
    ops = nullptr;
    return true;
  }
  const auto now = clock::now();
  if (!state->armed) {
    state->armed = true;
    state->fire_at = now + std::chrono::milliseconds(25);
  }
  if (now < state->fire_at) {
    // Deadline known, nothing due yet.
    deadline = state->fire_at;
    ops = nullptr;
    return true;
  }
  if (state->delivered || state->due_op == nullptr) {
    return false;
  }
  ops = state->due_op;
  state->delivered = true;
  return true;
}

// With a configured timer heap whose nearest deadline expires, the
// worker blocks in the bounded wait, wakes on -ETIME, and the due
// operation fetched after the timeout is delivered on the next pass.
TEST(IoUringErrorBranchTest, timer_deadline_expires_and_delivers_due_op) {
  io_uring_task_queue_state global_tasks;
  io_uring_context context;
  io_uring_context_options options;
  options.wait_spin_count = 4;
  if (!queue_init_shared_or_skip(context, global_tasks, options)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  timer_stub_state stub;

  receiver due_recv;
  due_recv.context = &context;
  due_recv.stop_on_completion = true;
  auto due_state = due_recv.state;
  auto due_op = std::make_unique<io_uring_post_operation<receiver>>(
      context, std::move(due_recv));
  stub.due_op = due_op.get();

  global_tasks.timeout_heap = &stub;
  global_tasks.try_fetch_timeout_operations = &timer_stub_fetch;

  context.run();

  EXPECT_EQ(due_state->signal, signal_kind::value)
      << "due timer operation never delivered";
  EXPECT_TRUE(due_state->in_context);
}

// With a configured but permanently empty timer heap the worker parks
// without a deadline and must still wake for externally posted work.
TEST(IoUringErrorBranchTest, empty_timer_heap_worker_wakes_on_posted_work) {
  io_uring_task_queue_state global_tasks;
  io_uring_context context;
  if (!queue_init_shared_or_skip(context, global_tasks)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  timer_stub_state stub;
  stub.always_empty = true;
  global_tasks.timeout_heap = &stub;
  global_tasks.try_fetch_timeout_operations = &timer_stub_fetch;

  receiver recv;
  recv.context = &context;
  recv.stop_on_completion = true;
  auto recv_state = recv.state;
  auto post_op = std::make_unique<io_uring_post_operation<receiver>>(
      context, std::move(recv));

  std::atomic<bool> run_done{false};
  std::thread worker([&context, &run_done] {
    context.run();
    run_done.store(true, std::memory_order_release);
  });

  ASSERT_TRUE(wait_until([&context] { return context.is_waiting(); }))
      << "worker never parked without a timer deadline";

  bexec::start(*post_op);

  ASSERT_TRUE(wait_until([&run_done] { return run_done.load(); }))
      << "worker never woke for the posted work";
  worker.join();

  EXPECT_EQ(recv_state->signal, signal_kind::value);
}

// Timer heap variant that answers true but never reports a deadline:
// the run loop must treat time_point::max() as "no deadline" and park
// without a bounded wait (the armed wake polls remain the wake source),
// still waking for externally posted work.
TEST(IoUringErrorBranchTest, timer_heap_without_deadline_parks_and_wakes) {
  io_uring_task_queue_state global_tasks;
  io_uring_context context;
  if (!queue_init_shared_or_skip(context, global_tasks)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  timer_stub_state stub;
  stub.no_deadline = true;
  global_tasks.timeout_heap = &stub;
  global_tasks.try_fetch_timeout_operations = &timer_stub_fetch;

  receiver recv;
  recv.context = &context;
  recv.stop_on_completion = true;
  auto recv_state = recv.state;
  auto post_op = std::make_unique<io_uring_post_operation<receiver>>(
      context, std::move(recv));

  std::atomic<bool> run_done{false};
  std::thread worker([&context, &run_done] {
    context.run();
    run_done.store(true, std::memory_order_release);
  });

  ASSERT_TRUE(wait_until([&context] { return context.is_waiting(); }))
      << "worker never parked without a timer deadline";

  bexec::start(*post_op);

  ASSERT_TRUE(wait_until([&run_done] { return run_done.load(); }))
      << "worker never woke for the posted work";
  worker.join();

  EXPECT_EQ(recv_state->signal, signal_kind::value);
}

// A directed wake through wake_one_sleeping() writes the worker's local
// wake channel; the resulting local-eventfd CQE must be collected (drain
// + re-arm branch) rather than lost, and the run loop finishes normally
// afterwards.
TEST(IoUringErrorBranchTest, local_directed_wake_collects_local_cqe) {
  io_uring_task_queue_state global_tasks;
  io_uring_context context;
  if (!queue_init_shared_or_skip(context, global_tasks)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  std::atomic<bool> run_done{false};
  std::thread worker([&context, &run_done] {
    context.run();
    run_done.store(true, std::memory_order_release);
  });

  ASSERT_TRUE(wait_until([&context] { return context.is_waiting(); }))
      << "worker never parked";
  ASSERT_TRUE(global_tasks.wake_one_sleeping())
      << "no sleeping worker found for the directed wake";

  // The worker consumes the local-eventfd CQE and parks again within
  // microseconds — far faster than is_waiting() could be observed
  // false — so simply give that pass time to run before finishing.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  context.stop();

  ASSERT_TRUE(wait_until([&run_done] { return run_done.load(); }));
  worker.join();
}

// Same directed wake, but stop() follows immediately: the local-eventfd
// CQE is then collected while the context is already stopping, so the
// in-collect re-arm of the local wake poll reports "not armed" (0)
// instead of resubmitting — the poll-state distinction introduced with
// the re-arm policy fix.
TEST(IoUringErrorBranchTest, stop_after_local_wake_rearms_local_poll_off) {
  io_uring_task_queue_state global_tasks;
  io_uring_context context;
  if (!queue_init_shared_or_skip(context, global_tasks)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  std::atomic<bool> run_done{false};
  std::thread worker([&context, &run_done] {
    context.run();
    run_done.store(true, std::memory_order_release);
  });

  ASSERT_TRUE(wait_until([&context] { return context.is_waiting(); }))
      << "worker never parked";
  // Let the worker reach io_uring_enter so the directed wake is
  // observed from inside the blocking wait, not the preamble.
  std::this_thread::sleep_for(std::chrono::milliseconds(2));

  // Directed wake first (worker still on the suspend list), stop right
  // after: the state flips to finishing before the worker can collect
  // the local-eventfd CQE, so its in-collect re-arm observes the
  // stopping state.
  ASSERT_TRUE(global_tasks.wake_one_sleeping());
  context.stop();

  ASSERT_TRUE(wait_until([&run_done] { return run_done.load(); }));
  worker.join();
}

// A timer heap configured without a fetch entry point parks the worker
// without a deadline (the heap probes short-circuit on the missing
// function) and posted work still wakes it.
TEST(IoUringErrorBranchTest, timer_heap_without_fetch_fn_parks_and_wakes) {
  io_uring_task_queue_state global_tasks;
  io_uring_context context;
  if (!queue_init_shared_or_skip(context, global_tasks)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  timer_stub_state stub;
  global_tasks.timeout_heap = &stub;
  global_tasks.try_fetch_timeout_operations = nullptr;

  receiver recv;
  recv.context = &context;
  recv.stop_on_completion = true;
  auto recv_state = recv.state;
  auto post_op = std::make_unique<io_uring_post_operation<receiver>>(
      context, std::move(recv));

  std::atomic<bool> run_done{false};
  std::thread worker([&context, &run_done] {
    context.run();
    run_done.store(true, std::memory_order_release);
  });

  ASSERT_TRUE(wait_until([&context] { return context.is_waiting(); }))
      << "worker never parked without a fetch entry point";

  bexec::start(*post_op);

  ASSERT_TRUE(wait_until([&run_done] { return run_done.load(); }));
  worker.join();

  EXPECT_EQ(recv_state->signal, signal_kind::value);
}

// Receiver that republishes one follow-up I/O operation when its own
// operation is aborted, exercising the nested-publish window inside
// abort_and_deliver_completions()'s consume loop.
struct republishing_poll_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  io_uring_context* context = nullptr;
  io_uring_io_operation_base* follow_up = nullptr;

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
    if (context != nullptr && follow_up != nullptr) {
      // publish_io() is legal here: the context is finishing (not yet
      // finished) during queue_exit()'s delivery phase.
      context->publish_io(*follow_up);
    }
  }
};

// queue_exit() on a context whose published I/O was never run: the
// abort delivers set_stopped synchronously, and a follow-up operation
// published from that receiver must itself be consumed, aborted, and
// delivered by the abort_and_deliver_completions() loop (never
// silently dropped).
TEST(IoUringErrorBranchTest, queue_exit_delivers_op_published_on_abort) {
  io_uring_task_queue_state global_tasks;
  std::unique_ptr<io_uring_poll_sender_operation<terminal_poll_receiver>>
      follow_up_op;
  std::unique_ptr<
      io_uring_poll_sender_operation<republishing_poll_receiver>>
      initial_op;
  io_uring_context context;
  if (!queue_init_shared_or_skip(context, global_tasks)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  int first_pipe[2] = {-1, -1};
  int second_pipe[2] = {-1, -1};
  ASSERT_EQ(::pipe2(first_pipe, O_CLOEXEC), 0);
  ASSERT_EQ(::pipe2(second_pipe, O_CLOEXEC), 0);

  terminal_poll_receiver follow_up_recv;
  follow_up_recv.context = &context;
  auto follow_up_state = follow_up_recv.state;
  follow_up_op =
      std::make_unique<io_uring_poll_sender_operation<terminal_poll_receiver>>(
          context, descriptor_view(second_pipe[0]),
          static_cast<unsigned>(POLLIN), std::move(follow_up_recv));

  republishing_poll_receiver initial_recv;
  initial_recv.context = &context;
  initial_recv.follow_up = follow_up_op.get();
  auto initial_state = initial_recv.state;
  initial_op = std::make_unique<
      io_uring_poll_sender_operation<republishing_poll_receiver>>(
      context, descriptor_view(first_pipe[0]),
      static_cast<unsigned>(POLLIN), std::move(initial_recv));
  bexec::start(*initial_op);

  context.queue_exit();

  EXPECT_EQ(initial_state->signal, signal_kind::stopped)
      << "initial published op must be aborted and delivered";
  EXPECT_TRUE(follow_up_state->signal == signal_kind::stopped ||
              follow_up_state->signal == signal_kind::error)
      << "follow-up op published from the aborted receiver was dropped "
         "instead of being aborted and delivered";

  (void)::close(first_pipe[0]);
  (void)::close(first_pipe[1]);
  (void)::close(second_pipe[0]);
  (void)::close(second_pipe[1]);
}

// Receiver counting poll completions; stops the context on the last one.
struct counting_poll_receiver {
  std::shared_ptr<batch_state> state = std::make_shared<batch_state>();
  io_uring_context* context = nullptr;
  unsigned target = 0;

  void set_value(std::error_code ec, unsigned) noexcept {
    if (ec) {
      ++state->errors;
    } else {
      ++state->completed;
    }
    if ((ec || state->completed == target) && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    ++state->stopped;
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

// Three poll completions arriving while the worker is parked are
// collected as one CQE batch; with a zero inline threshold and a local
// budget of two, the batch exceeds the budget and spills to the shared
// CPU queue (dispatch tier 3). Stealing no longer exists, so the fetch
// path is just local → shared.
TEST(IoUringErrorBranchTest, multi_cqe_batch_spills_to_shared_cpu_queue) {
  io_uring_task_queue_state global_tasks;
  io_uring_context context;
  io_uring_context_options options;
  options.cqe_inline_completion_threshold = 0;
  options.local_queue_threshold = 2;
  if (!queue_init_shared_or_skip(context, global_tasks, options)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  constexpr unsigned k_ops = 3;
  std::vector<std::unique_ptr<
      io_uring_poll_sender_operation<counting_poll_receiver>>>
      ops;
  auto state = std::make_shared<batch_state>();
  std::vector<int> read_fds;
  std::vector<int> write_fds;
  for (unsigned index = 0; index < k_ops; ++index) {
    int descriptors[2] = {-1, -1};
    ASSERT_EQ(::pipe2(descriptors, O_CLOEXEC), 0);
    read_fds.push_back(descriptors[0]);
    write_fds.push_back(descriptors[1]);

    counting_poll_receiver recv;
    recv.context = &context;
    recv.target = k_ops;
    recv.state = state;
    ops.push_back(std::make_unique<
                  io_uring_poll_sender_operation<counting_poll_receiver>>(
        context, descriptor_view(descriptors[0]),
        static_cast<unsigned>(POLLIN), std::move(recv)));
    bexec::start(*ops.back());
  }

  std::atomic<bool> run_done{false};
  std::thread worker([&context, &run_done] {
    context.run();
    run_done.store(true, std::memory_order_release);
  });

  // Wait until all three operations are inflight and the worker parked.
  ASSERT_TRUE(wait_until([&context] { return context.is_waiting(); }))
      << "worker never parked with three inflight polls";

  // Complete all three while parked so their CQEs are collected in one
  // batch.
  for (int fd : write_fds) {
    ASSERT_EQ(::write(fd, "x", 1), 1);
  }

  ASSERT_TRUE(wait_until([&run_done] { return run_done.load(); }))
      << "worker never drained the spilled CQE batch";
  worker.join();

  EXPECT_EQ(state->completed, k_ops);
  EXPECT_EQ(state->errors, 0u);
  EXPECT_EQ(state->stopped, 0u);

  for (int fd : read_fds) {
    (void)::close(fd);
  }
  for (int fd : write_fds) {
    (void)::close(fd);
  }
}

// queue_init() validation: zero entries are rejected with -EINVAL both
// with the default setup flags (exercising the fallback retry) and with
// plain flags (no retry).
TEST(IoUringErrorBranchTest, queue_init_rejects_zero_entries) {
  // Probe plain ring support once; skip cleanly when unavailable.
  {
    io_uring_task_queue_state probe_state;
    io_uring_context probe;
    reset_task_queue_state(probe_state);
    probe.set_global_state(&probe_state);
    const int probe_result = probe.queue_init(io_uring_context_options{});
    if (is_unsupported_ring_error(probe_result)) {
      GTEST_SKIP() << "io_uring is unavailable (queue_init="
                   << probe_result << ")";
    }
  }

  io_uring_context with_flags;
  io_uring_context_options options;
  options.entries = 0;
  EXPECT_EQ(with_flags.queue_init(options), -EINVAL)
      << "zero entries unexpectedly accepted (default flags)";

  io_uring_context without_flags;
  io_uring_context_options plain_options;
  plain_options.entries = 0;
  plain_options.setup_flags = 0;
  EXPECT_EQ(without_flags.queue_init(plain_options), -EINVAL)
      << "plain-flags init must fail fast without the fallback retry";
}

// queue_init() is single-shot: a second initialization returns
// -EALREADY.
TEST(IoUringErrorBranchTest, queue_init_twice_returns_ealready) {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }
  EXPECT_EQ(context.queue_init(io_uring_context_options{}), -EALREADY);
}

// Options branches: a zero CQE batch window is normalized to one, and
// plain setup flags (no SINGLE_ISSUER, hence no R_DISABLED) initialize
// a working ring.
TEST(IoUringErrorBranchTest, queue_init_accepts_zero_window_plain_flags) {
  io_uring_task_queue_state global_tasks;
  io_uring_context context;
  io_uring_context_options options;
  options.cqe_batch_window = 0;
  options.setup_flags = 0;
  if (!queue_init_shared_or_skip(context, global_tasks, options)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  receiver recv;
  recv.context = &context;
  recv.stop_on_completion = true;
  auto recv_state = recv.state;
  auto post_op = std::make_unique<io_uring_post_operation<receiver>>(
      context, std::move(recv));
  bexec::start(*post_op);

  context.run();

  EXPECT_EQ(recv_state->signal, signal_kind::value);
}

}  // namespace
