#include <bnio/bnio.h>
#include <gtest/gtest.h>

#include <atomic>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <memory>
#include <thread>

namespace {

std::atomic<bool> g_worker_entered{false};

// Receiver that sets g_worker_entered once the posted schedule()
// completes inside the worker thread.  This guarantees the flag
// is only set after running_workers_.fetch_add(1) has executed,
// closing the use-after-free window observed in the original test.
struct worker_entered_receiver {
  std::atomic<bool>* flag;
  void set_value(std::error_code /*ec*/) noexcept {
    flag->store(true, std::memory_order_release);
  }
  void set_stopped() noexcept { flag->store(true, std::memory_order_release); }
};

TEST(LifecycleTest, stop_run_uaf_stress) {
  // Verify that stop() correctly waits for workers that have entered run(),
  // and the native context destructor doesn't access freed memory.
  // Actual UAF detection is done by ASAN at runtime.
  constexpr int iterations = 20000;

  {
    bnio::io_context probe;
    if (!probe.is_open()) {
      GTEST_SKIP() << "native I/O context is unavailable";
    }
  }

  int completed = 0;

  for (int iter = 0; iter < iterations; iter++) {
    g_worker_entered.store(false, std::memory_order_release);

    auto ctx = std::make_unique<bnio::io_context>();
    if (!ctx->is_open()) {
      continue;
    }

    // Post a task that will run inside the worker's event loop.
    // This guarantees that when g_worker_entered becomes true,
    // running_workers_ has already been incremented by run().
    auto scheduler = ctx->get_post_scheduler();
    auto sender = scheduler.schedule();
    auto op =
        bexec::connect(sender, worker_entered_receiver{&g_worker_entered});
    bexec::start(op);

    std::thread worker([&ctx]() { ctx->run(); });

    // Spin until the worker has entered the event loop and
    // processed the posted task.
    while (!g_worker_entered.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    // Safe: the worker is inside run() with running_workers_ > 0.
    // stop() will wait until the worker observes the closing flag
    // and exits the run loop.
    ctx->stop();

    // Safe: stop() is guaranteed to have waited for the worker.
    // Safe: native_context destructor won't access global_state_
    //       because set_global_state(nullptr) was called before
    //       running_workers_.fetch_sub(1).
    ctx.reset();

    worker.join();
    completed++;
  }

  EXPECT_EQ(completed, iterations);
}

}  // namespace
