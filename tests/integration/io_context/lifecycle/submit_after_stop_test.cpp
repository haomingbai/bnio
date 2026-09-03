#include <bnio/bnio.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <system_error>
#include <thread>

namespace {

// Classifies terminal receiver calls.  The shutdown contract: a schedule()
// submitted after stop() is rejected and completes inline via
// set_value(operation_canceled), never via set_stopped.
struct counting_recv {
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

// Issue 1 (stranded operations): a schedule() operation submitted after
// stop() has drained the workers must still complete. publish_cpu rejects
// the enqueue when the context is stopping (documented: publish assumes a
// non-stopped context) and the caller completes the operation inline with a
// canceled completion. The lock-free baseline enqueues into a queue that no
// worker ever drains again, so the receiver never completes.
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
  std::atomic<int> canceled{0};
  std::atomic<int> stopped{0};
  auto scheduler = ctx->get_post_scheduler();
  auto operation = bexec::connect(
      scheduler.schedule(), counting_recv{&completed, &canceled, &stopped});
  bexec::start(operation);

  // Bounded wait: a stranded operation would never complete.
  for (int i = 0; i < 100 && completed.load(std::memory_order_acquire) == 0;
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(completed.load(std::memory_order_acquire), 1);
  // The publish lands after stop(): it must be rejected and complete
  // inline via set_value(operation_canceled), never set_stopped.
  EXPECT_EQ(canceled.load(std::memory_order_acquire), 1);
  EXPECT_EQ(stopped.load(std::memory_order_acquire), 0);
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
  std::atomic<int> canceled{0};
  std::atomic<int> stopped{0};
  auto scheduler = ctx->get_dispatch_scheduler();
  auto operation = bexec::connect(
      scheduler.schedule(), counting_recv{&completed, &canceled, &stopped});
  bexec::start(operation);

  for (int i = 0; i < 100 && completed.load(std::memory_order_acquire) == 0;
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(completed.load(std::memory_order_acquire), 1);
  EXPECT_EQ(canceled.load(std::memory_order_acquire), 1);
  EXPECT_EQ(stopped.load(std::memory_order_acquire), 0);
}

}  // namespace
