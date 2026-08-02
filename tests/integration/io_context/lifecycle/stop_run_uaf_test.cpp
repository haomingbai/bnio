#include <gtest/gtest.h>

#include <bnio/bnio.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace {

std::atomic<bool> g_worker_entered{false};

void worker_func(bnio::io_context* ctx) {
  g_worker_entered.store(true, std::memory_order_release);
  ctx->run();
}

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

    std::thread worker([&ctx]() { worker_func(ctx.get()); });

    // Spin until the worker signals it has entered the worker function.
    // The first instruction of run() is running_workers.fetch_add(1),
    // so once worker_func is entered, stop() will see this worker.
    while (!g_worker_entered.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    // Give the worker a tiny yield to actually reach fetch_add inside run()
    std::this_thread::yield();

    // stop() blocks until the worker exits.
    ctx->stop();

    // Safe: stop() waited for worker to exit.
    // Safe: native_context destructor won't access global_state_
    //       because set_global_state(nullptr) was called before fetch_sub.
    ctx.reset();

    worker.join();
    completed++;
  }

  EXPECT_EQ(completed, iterations);
}

}  // namespace
