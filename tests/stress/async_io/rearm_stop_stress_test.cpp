/**
 * @file rearm_stop_stress_test.cpp
 * @brief Stress test for the wake-channel re-arm failure under SQ pressure
 * that leaves a worker parked without a wake source.
 *
 * Defect under test (pre-fix behavior):
 *   When the eventfd poll CQE is collected while the submission queue is
 *   full (tiny ring + SQPOLL, so io_uring_submit() cannot free SQ slots
 *   synchronously), the in-collect re-arm submit_eventfd_poll() returns
 *   -EAGAIN and collect_cqe_tasks() immediately stores
 *   context_state::finishing.  The half-closed state conflates "poll
 *   already armed" with "poll not armed" in submit_eventfd_poll()/submit_
 *   local_eventfd_poll() (both return 0 when state != running), so the
 *   worker later parks in io_uring_enter(GETEVENTS) with no pending wake
 *   poll.  An io_context::stop() then writes the shared wake channel but
 *   no poll SQE exists to turn that write into a CQE: the enter never
 *   returns and the stop hangs.  In-flight and queued operations are also
 *   stranded without a terminal receiver call.
 *
 * Scenario per round (fresh io_uring_task_queue_state + io_uring_context,
 * single worker):
 *   - producers publish a bounded flood of async_poll operations on a
 *     never-ready pipe read end plus posted operations, occasionally
 *     writing the shared wake channel unconditionally (mirroring
 *     io_context::stop_internal()'s wake loop, which is not gated on
 *     is_waiting());
 *   - the flood keeps the 4-entry SQ full for extended periods, so
 *     eventfd poll CQEs get collected while re-arm attempts must fail;
 *   - then an io_context-style stop runs: life_state = 1 plus repeated
 *     unconditional wake-channel writes until the worker exits.
 *
 * Failure detectors:
 *   - the worker fails to exit within the bounded join (worker hang), or
 *   - the worker exits but any started operation's receiver never got a
 *     terminal call (stranded operations).
 *
 * A variant without SQPOLL runs the same shape with a heavier flood as a
 * regression guard for environments where SQPOLL is unavailable.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "../../support/async_io/io_uring_context_test_support.h"

namespace {

using namespace bnio_async_io_io_uring_test;

// Bounded wait for the worker to exit after the stop sequence.  A healthy
// stop completes in milliseconds; only a worker hang can exceed this.
constexpr auto kWorkerJoinBound = std::chrono::seconds(5);

struct op_holder_base {
  virtual ~op_holder_base() = default;
  virtual void start() = 0;
};

// Operation state produced by bexec::connect (poll senders).
template <typename Op>
struct op_holder : op_holder_base {
  template <typename Sender, typename Receiver>
  op_holder(Sender&& s, Receiver&& r)
      : op(bexec::connect(std::forward<Sender>(s), std::forward<Receiver>(r))) {
  }
  void start() override { bexec::start(op); }
  Op op;
};

// Operation state constructed directly (posted operations).
template <typename Op>
struct direct_op_holder : op_holder_base {
  template <typename... Args>
  explicit direct_op_holder(Args&&... args)
      : op(std::forward<Args>(args)...) {}
  void start() override { bexec::start(op); }
  Op op;
};

// Counts terminal receiver calls: both set_value variants and set_stopped.
struct flood_receiver {
  std::atomic<unsigned>* terminal = nullptr;

  void set_value(std::error_code) noexcept {
    terminal->fetch_add(1, std::memory_order_acq_rel);
  }

  void set_value(std::error_code, unsigned) noexcept {
    terminal->fetch_add(1, std::memory_order_acq_rel);
  }

  void set_stopped() noexcept {
    terminal->fetch_add(1, std::memory_order_acq_rel);
  }
};

struct round_outcome {
  bool hung = false;         // worker never exited within kWorkerJoinBound
  unsigned started = 0;      // operations handed to the context
  unsigned terminal = 0;     // operations that reached a receiver call
};

// Owns one round's whole lifetime.  Heap-allocated so a hung round can be
// intentionally leaked: the parked worker thread then keeps referencing
// valid memory instead of destroyed stack frames.
struct round_owner {
  // Destruction order matters: the context's queue_exit() aborts any
  // still-queued or inflight operations (touching them), so the operation
  // holders must outlive the context, and the pipe fds must outlive the
  // ring whose pending kernel poll requests reference the file.  Members
  // are therefore destroyed in the reverse of the order below.
  std::vector<std::unique_ptr<op_holder_base>> ops;
  int never_ready_fd = -1;
  int pipe_write_fd = -1;
  io_uring_task_queue_state global_tasks;
  io_uring_context context;
  std::atomic<bool> worker_done{false};
  std::atomic<unsigned> started{0};
  std::atomic<unsigned> terminal{0};

  ~round_owner() {
    if (never_ready_fd >= 0) {
      (void)::close(never_ready_fd);
    }
    if (pipe_write_fd >= 0) {
      (void)::close(pipe_write_fd);
    }
  }
};

round_outcome run_stop_round(const io_uring_context_options& options,
                             unsigned poll_ops, unsigned post_ops,
                             unsigned producer_count,
                             unsigned wake_stride) {
  round_outcome outcome;

  auto owner = std::make_unique<round_owner>();
  int pipe_fds[2] = {-1, -1};
  if (::pipe(pipe_fds) != 0) {
    ADD_FAILURE() << "pipe() failed: " << errno;
    return outcome;
  }
  owner->never_ready_fd = pipe_fds[0];
  owner->pipe_write_fd = pipe_fds[1];

  // Fresh shared task-queue state per round: never reuse state across
  // rounds, exactly like io_context owning one state per instance.
  reset_task_queue_state(owner->global_tasks);
  owner->context.set_global_state(&owner->global_tasks);
  const int init_result = owner->context.queue_init(options);
  if (init_result < 0) {
    ADD_FAILURE() << "queue_init failed mid-stress: " << init_result;
    return outcome;
  }

  // Pre-create every operation so producer threads only ever call start().
  for (unsigned i = 0; i < poll_ops; ++i) {
    auto sender =
        owner->context.async_poll(descriptor_view(owner->never_ready_fd),
                                  static_cast<unsigned>(POLLIN));
    using poll_op_t =
        decltype(bexec::connect(sender, flood_receiver{nullptr}));
    owner->ops.push_back(std::make_unique<op_holder<poll_op_t>>(
        sender, flood_receiver{&owner->terminal}));
  }
  for (unsigned i = 0; i < post_ops; ++i) {
    owner->ops.push_back(std::make_unique<
        direct_op_holder<io_uring_post_operation<flood_receiver>>>(
        owner->context, flood_receiver{&owner->terminal}));
  }
  const unsigned total_ops = poll_ops + post_ops;

  // Single worker per round.
  std::thread worker([owner_ptr = owner.get()]() {
    owner_ptr->context.run();
    owner_ptr->worker_done.store(true, std::memory_order_release);
  });
  worker.detach();

  std::vector<std::thread> producers;
  for (unsigned p = 0; p < producer_count; ++p) {
    producers.emplace_back([owner_ptr = owner.get(), p, producer_count,
                            wake_stride, total_ops]() {
      unsigned published = 0;
      for (unsigned i = p; i < total_ops; i += producer_count) {
        // Stop publishing once the worker has exited.  Publishing into a
        // fully finished context is outside this test's scope (the
        // internal submission contract) and would trip assert_running()
        // in debug builds.
        if (owner_ptr->worker_done.load(std::memory_order_acquire)) {
          break;
        }
        owner_ptr->started.fetch_add(1, std::memory_order_acq_rel);
        owner_ptr->ops[i]->start();
        ++published;
        // External wake pressure: io_context::stop_internal() writes the
        // shared wake channel unconditionally (not gated on is_waiting()),
        // so eventfd poll CQEs must be re-armed while the submission queue
        // is under pressure, not only while the worker happens to wait.
        if (wake_stride != 0 && published % wake_stride == 0) {
          (void)owner_ptr->global_tasks.wake_channel_.wake();
        }
        std::this_thread::yield();
      }
    });
  }
  for (auto& producer : producers) {
    producer.join();
  }

  // io_context::stop() semantics: publish the closing flag first, then
  // keep writing the shared wake channel until the worker exits —
  // io_context::stop_internal()'s wake loop.
  owner->global_tasks.life_state.store(1, std::memory_order_release);
  (void)owner->context.stop();

  const auto deadline =
      std::chrono::steady_clock::now() + kWorkerJoinBound;
  while (!owner->worker_done.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    (void)owner->global_tasks.wake_channel_.wake();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  outcome.started = owner->started.load(std::memory_order_acquire);
  if (!owner->worker_done.load(std::memory_order_acquire)) {
    // The hang: the worker is parked in io_uring_enter with no armed
    // wake poll, so no channel write can produce a CQE.  Leak the whole
    // round so the parked thread never touches freed memory; the test
    // stops after the first hang to bound the leak to one round.
    outcome.hung = true;
    (void)owner.release();
    return outcome;
  }
  outcome.terminal = owner->terminal.load(std::memory_order_acquire);
  return outcome;
}

struct stress_summary {
  unsigned rounds = 0;
  unsigned hang_rounds = 0;      // worker never exited (worker hang)
  unsigned stranded_rounds = 0;  // worker exited, ops left uncompleted
  int first_bad_round = -1;
  unsigned first_bad_started = 0;
  unsigned first_bad_terminal = 0;
  bool first_bad_hung = false;
};

stress_summary run_rounds(const io_uring_context_options& options,
                          unsigned rounds, unsigned poll_ops,
                          unsigned post_ops, unsigned producer_count,
                          unsigned wake_stride) {
  stress_summary summary;
  for (unsigned round = 0; round < rounds; ++round) {
    const round_outcome outcome = run_stop_round(
        options, poll_ops, post_ops, producer_count, wake_stride);
    ++summary.rounds;
    const bool bad = outcome.hung || outcome.terminal != outcome.started;
    if (!bad) {
      continue;
    }
    if (outcome.hung) {
      ++summary.hang_rounds;
    } else {
      ++summary.stranded_rounds;
    }
    if (summary.first_bad_round < 0) {
      summary.first_bad_round = static_cast<int>(round);
      summary.first_bad_started = outcome.started;
      summary.first_bad_terminal = outcome.terminal;
      summary.first_bad_hung = outcome.hung;
    }
    if (outcome.hung) {
      // Each hung round leaks one parked worker; one is enough evidence.
      break;
    }
  }
  return summary;
}

// SQPOLL variant: the primary reproduction of the re-arm failure.
// io_uring_submit() cannot free SQ-ring slots synchronously under SQPOLL
// (only the kernel poll thread advances sq head), so a full 4-entry SQ
// makes the in-collect re-arm return -EAGAIN.
TEST(RearmStopStressTest, stop_completes_all_ops_under_sqpoll_rearm_pressure) {
  io_uring_context_options options;
  options.entries = 4;
  options.enable_sqpoll = true;
  options.sqpoll_idle_ms = 10;

  // Probe SQPOLL availability once; skip cleanly when the host denies it.
  // The non-SQPOLL fallback variant in this file still runs.
  {
    io_uring_task_queue_state probe_state;
    io_uring_context probe;
    reset_task_queue_state(probe_state);
    probe.set_global_state(&probe_state);
    const int probe_result = probe.queue_init(options);
    if (probe_result < 0) {
      if (probe_result == -EPERM || probe_result == -EACCES ||
          probe_result == -ENOSYS || probe_result == -EINVAL) {
        GTEST_SKIP() << "SQPOLL io_uring unavailable on this host "
                     << "(queue_init=" << probe_result
                     << "); the non-SQPOLL fallback variant still runs";
      }
      FAIL() << "unexpected SQPOLL queue_init failure: " << probe_result;
    }
  }

  constexpr unsigned kRounds = 200;
  constexpr unsigned kPollOps = 64;
  constexpr unsigned kPostOps = 16;
  constexpr unsigned kProducers = 4;
  constexpr unsigned kWakeStride = 2;

  const stress_summary summary = run_rounds(options, kRounds, kPollOps,
                                            kPostOps, kProducers,
                                            kWakeStride);

  EXPECT_EQ(summary.hang_rounds, 0u)
      << "worker parked in io_uring_enter without an armed wake poll; the "
      << "io_context-style stop() wrote the channel but no poll SQE could "
      << "turn it into a CQE (worker hang). First bad round "
      << summary.first_bad_round << ": hung=" << summary.first_bad_hung
      << ", started=" << summary.first_bad_started;
  EXPECT_EQ(summary.stranded_rounds, 0u)
      << "worker exited but started operations never reached a terminal "
      << "receiver call (stranded operations). First bad round "
      << summary.first_bad_round << ": started="
      << summary.first_bad_started
      << ", terminal=" << summary.first_bad_terminal;
}

// Fallback variant without SQPOLL: same shape with a heavier flood.  Runs
// even where SQPOLL is unavailable; without SQPOLL the submit path flushes
// SQ entries synchronously, so the re-arm -EAGAIN window is narrow and this
// variant primarily guards the stop/stranding invariants.
TEST(RearmStopStressTest, stop_completes_all_ops_fallback_no_sqpoll) {
  io_uring_context_options options;
  options.entries = 4;

  // Probe plain io_uring availability once.
  {
    io_uring_task_queue_state probe_state;
    io_uring_context probe;
    reset_task_queue_state(probe_state);
    probe.set_global_state(&probe_state);
    const int probe_result = probe.queue_init(options);
    if (probe_result < 0) {
      if (is_unsupported_ring_error(probe_result)) {
        GTEST_SKIP() << "io_uring unavailable on this host (queue_init="
                     << probe_result << ")";
      }
      FAIL() << "unexpected queue_init failure: " << probe_result;
    }
  }

  constexpr unsigned kRounds = 200;
  constexpr unsigned kPollOps = 128;
  constexpr unsigned kPostOps = 32;
  constexpr unsigned kProducers = 4;
  constexpr unsigned kWakeStride = 2;

  const stress_summary summary = run_rounds(options, kRounds, kPollOps,
                                            kPostOps, kProducers,
                                            kWakeStride);

  EXPECT_EQ(summary.hang_rounds, 0u)
      << "worker parked in io_uring_enter without an armed wake poll "
      << "(worker hang). First bad round " << summary.first_bad_round
      << ": hung=" << summary.first_bad_hung
      << ", started=" << summary.first_bad_started;
  EXPECT_EQ(summary.stranded_rounds, 0u)
      << "worker exited but started operations never reached a terminal "
      << "receiver call (stranded operations). First bad round "
      << summary.first_bad_round << ": started="
      << summary.first_bad_started
      << ", terminal=" << summary.first_bad_terminal;
}

}  // namespace
