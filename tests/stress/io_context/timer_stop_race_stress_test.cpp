/**
 * @file timer_stop_race_stress_test.cpp
 * @brief Stress test that amplifies the timer-stop race condition.
 *
 * Race description: When io_context::stop() calls begin_stop() before
 * abort_pending_timer_waits(), a worker that is actively spinning (not
 * sleeping in a syscall) can observe life_state == 1, enter finish(), drain
 * the still-empty timers_.ready, and exit — all before
 * abort_pending_timer_waits() moves operations to timers_.ready.
 *
 * Amplification strategy:
 * 1. Use many short-lived timers to keep the worker busy in
 *    handle_run_ready_tasks() / spin_for_work()
 * 2. Submit a burst of far-future timers (the ones that should be
 *    cancelled by stop) immediately before calling stop()
 * 3. Run many iterations to increase the probability of hitting the
 *    scheduling window
 */

#include <bnio/bnio.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

namespace {

struct op_holder_base {
  virtual ~op_holder_base() = default;
};

template <typename Op>
struct op_holder : op_holder_base {
  Op op;
  template <typename Sender, typename Receiver>
  explicit op_holder(Sender&& s, Receiver&& r)
      : op(bexec::connect(std::forward<Sender>(s), std::forward<Receiver>(r))) {
  }
};

// Classifies terminal receiver calls instead of lumping them together.
// The lifecycle contract: context stop() aborting a pending timer wait
// completes via set_value(operation_canceled), never via set_stopped.
struct timer_recv {
  std::atomic<int>* counter = nullptr;   // every terminal call
  std::atomic<int>* canceled = nullptr;  // set_value(operation_canceled)
  std::atomic<int>* stopped = nullptr;   // set_stopped (contract violation)
  void set_value(std::error_code ec) noexcept {
    if (counter) counter->fetch_add(1, std::memory_order_relaxed);
    if (canceled && ec == std::make_error_code(std::errc::operation_canceled)) {
      canceled->fetch_add(1, std::memory_order_relaxed);
    }
  }
  void set_stopped() noexcept {
    if (counter) counter->fetch_add(1, std::memory_order_relaxed);
    if (stopped) stopped->fetch_add(1, std::memory_order_relaxed);
  }
};

// Test A: Repeated stop while pending timers exist.
// Each iteration starts timers with far-future expiry, then immediately
// stops.  Running many iterations amplifies the scheduling race.
TEST(TimerStopRaceStressTest, repeated_stop_with_pending_timers) {
  constexpr int kIterations = 200;
  constexpr int kTimersPerIteration = 20;

  int failures = 0;

  for (int iter = 0; iter < kIterations; ++iter) {
    auto ctx = std::make_unique<bnio::io_context>();
    if (!ctx->is_open()) {
      GTEST_SKIP() << "native I/O context is unavailable";
    }

    std::atomic<int> completed{0};
    std::atomic<int> canceled{0};
    std::atomic<int> stopped{0};
    std::vector<std::unique_ptr<op_holder_base>> ops;
    std::vector<bnio::steady_timer> timers;

    for (int i = 0; i < kTimersPerIteration; ++i) {
      timers.emplace_back(*ctx);
    }

    // Start the worker and wait for it to enter the run loop.
    std::thread worker([&ctx]() { ctx->run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Set all timers far in the future.  The first expires_at() call
    // wakes the worker (heap_deadline changes from max() to far-future),
    // bringing it into handle_run_ready_tasks() / spin_for_work() —
    // exactly where we want it when stop() is called.
    auto far = std::chrono::steady_clock::now() + std::chrono::hours(24);
    for (auto& timer : timers) {
      (void)timer.expires_at(far);
      auto sender = timer.async_wait();
      using Op = decltype(bexec::connect(sender, timer_recv{nullptr, nullptr}));
      auto h = std::make_unique<op_holder<Op>>(
          sender, timer_recv{&completed, &canceled, &stopped});
      bexec::start(h->op);
      ops.push_back(std::move(h));
    }

    // stop() triggers the race: begin_stop() sets life_state=1, and if
    // the worker observes it before abort_pending_timer_waits() drains
    // the timers, ops are stranded.
    ctx->stop();
    worker.join();

    if (completed.load() != kTimersPerIteration) {
      ++failures;
    }
    // Every far-future timer is pending when stop() lands, so every one
    // must complete through set_value(operation_canceled).
    if (canceled.load() != kTimersPerIteration) {
      ++failures;
    }
    if (stopped.load() != 0) {
      ++failures;
    }
  }

  EXPECT_EQ(failures, 0)
      << failures << " / " << kIterations
      << " iterations had stranded timer operations, a missing "
         "operation_canceled abort, or a contract-violating set_stopped";
}

// Test B: Burst-stop pattern — submit many far-future timers while the
// worker is processing short-lived timers, then stop immediately.
// This keeps the worker in the run_ready_tasks phase, maximizing the
// chance that it sees life_state == 1 before abort_pending_timer_waits().
TEST(TimerStopRaceStressTest, burst_stop_during_active_worker) {
  constexpr int kIterations = 100;
  constexpr int kBurstTimers = 50;
  constexpr int kBusyTimers = 20;

  int failures = 0;

  for (int iter = 0; iter < kIterations; ++iter) {
    auto ctx = std::make_unique<bnio::io_context>();
    if (!ctx->is_open()) {
      GTEST_SKIP() << "native I/O context is unavailable";
    }

    std::atomic<int> burst_completed{0};
    std::atomic<int> busy_completed{0};
    std::atomic<int> burst_canceled{0};
    std::atomic<int> stopped{0};
    std::vector<std::unique_ptr<op_holder_base>> burst_ops;
    std::vector<std::unique_ptr<op_holder_base>> busy_ops;
    std::vector<bnio::steady_timer> burst_timers;
    std::vector<bnio::steady_timer> busy_timers;

    for (int i = 0; i < kBurstTimers; ++i) {
      burst_timers.emplace_back(*ctx);
    }
    for (int i = 0; i < kBusyTimers; ++i) {
      busy_timers.emplace_back(*ctx);
    }

    // Start worker.
    std::thread worker([&ctx]() { ctx->run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Submit busy timers with very short expiry — they will expire
    // and keep the worker active in handle_run_ready_tasks().
    for (auto& timer : busy_timers) {
      (void)timer.expires_after(std::chrono::microseconds(1));
      auto sender = timer.async_wait();
      using Op = decltype(bexec::connect(sender, timer_recv{nullptr, nullptr}));
      auto h = std::make_unique<op_holder<Op>>(
          sender, timer_recv{&busy_completed, nullptr, &stopped});
      bexec::start(h->op);
      busy_ops.push_back(std::move(h));
    }

    // Submit burst timers with far-future expiry — stop() aborts these
    // pending waits through set_value(operation_canceled).
    auto far = std::chrono::steady_clock::now() + std::chrono::hours(24);
    for (auto& timer : burst_timers) {
      (void)timer.expires_at(far);
      auto sender = timer.async_wait();
      using Op = decltype(bexec::connect(sender, timer_recv{nullptr, nullptr}));
      auto h = std::make_unique<op_holder<Op>>(
          sender, timer_recv{&burst_completed, &burst_canceled, &stopped});
      bexec::start(h->op);
      burst_ops.push_back(std::move(h));
    }

    // Immediate stop — worker may be in handle_run_ready_tasks when
    // life_state flips.
    ctx->stop();
    worker.join();

    // Every started operation must reach a terminal receiver call.
    if (burst_completed.load() != kBurstTimers) {
      ++failures;
    }
    if (busy_completed.load() != kBusyTimers) {
      ++failures;
    }
    // All burst timers are still pending at stop(): each must abort via
    // set_value(operation_canceled).  Busy timers may have expired
    // already, so their split between normal completion and cancellation
    // is timing-dependent and only the terminal count is asserted.
    if (burst_canceled.load() != kBurstTimers) {
      ++failures;
    }
    if (stopped.load() != 0) {
      ++failures;
    }
  }

  EXPECT_EQ(failures, 0)
      << failures << " / " << kIterations
      << " iterations had stranded timer operations, a missing "
         "operation_canceled abort, or a contract-violating set_stopped";
}

}  // namespace
