#include <bnio/bnio.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace {

struct counting_recv {
  std::atomic<int>* counter = nullptr;
  void set_value(std::error_code) noexcept {
    if (counter) counter->fetch_add(1, std::memory_order_relaxed);
  }
  void set_stopped() noexcept {
    if (counter) counter->fetch_add(1, std::memory_order_relaxed);
  }
};

// Issue 1 (stranded operations): a schedule() operation submitted after
// stop() has drained the workers must still complete. publish_cpu rejects
// the enqueue when the context is stopping (documented: publish assumes a
// non-stopped context) and the caller completes the operation inline with
// set_stopped. The lock-free baseline enqueues into a queue that no worker
// ever drains again, so the receiver never completes.
TEST(LifecycleTest, post_schedule_after_stop_completes_inline) {
  auto ctx = std::make_unique<bnio::io_context>();
  if (!ctx->is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  std::thread worker([&ctx]() { ctx->run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  (void)ctx->stop();
  worker.join();

  std::atomic<int> completed{0};
  auto scheduler = ctx->get_post_scheduler();
  auto operation =
      bexec::connect(scheduler.schedule(), counting_recv{&completed});
  bexec::start(operation);

  // Bounded wait: a stranded operation would never complete.
  for (int i = 0; i < 100 && completed.load(std::memory_order_acquire) == 0;
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(completed.load(std::memory_order_acquire), 1);
}

// Same guarantee for the dispatch scheduler when start() runs on a
// non-context thread (it falls through to the same publish_cpu path).
TEST(LifecycleTest, dispatch_schedule_after_stop_completes_inline) {
  auto ctx = std::make_unique<bnio::io_context>();
  if (!ctx->is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  std::thread worker([&ctx]() { ctx->run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  (void)ctx->stop();
  worker.join();

  std::atomic<int> completed{0};
  auto scheduler = ctx->get_dispatch_scheduler();
  auto operation =
      bexec::connect(scheduler.schedule(), counting_recv{&completed});
  bexec::start(operation);

  for (int i = 0; i < 100 && completed.load(std::memory_order_acquire) == 0;
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(completed.load(std::memory_order_acquire), 1);
}

}  // namespace
