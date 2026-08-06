#include <bnio/bnio.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
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

struct counting_recv {
  std::atomic<int>* counter = nullptr;
  void set_value(std::error_code) noexcept {
    if (counter) counter->fetch_add(1, std::memory_order_relaxed);
  }
  void set_stopped() noexcept {
    if (counter) counter->fetch_add(1, std::memory_order_relaxed);
  }
};

struct timer_recv {
  std::atomic<int>* counter = nullptr;
  void set_value(std::error_code) noexcept {
    if (counter) counter->fetch_add(1, std::memory_order_relaxed);
  }
  void set_stopped() noexcept {
    if (counter) counter->fetch_add(1, std::memory_order_relaxed);
  }
};

// Test 1: schedule() operations from non-worker thread while stopping.
// All 200 schedule operations must complete.
TEST(LifecycleTest, stop_completes_all_schedule_ops) {
  auto ctx = std::make_unique<bnio::io_context>();
  if (!ctx->is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  constexpr int N = 200;
  std::atomic<int> completed{0};
  std::vector<std::unique_ptr<op_holder_base>> ops;
  ops.reserve(N);

  std::thread worker([&ctx]() { ctx->run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto sched = ctx->get_post_scheduler();
  using Sender = decltype(sched.schedule());
  using Op =
      decltype(bexec::connect(std::declval<Sender>(), counting_recv{nullptr}));

  std::thread poster([&]() {
    for (int i = 0; i < N; i++) {
      auto sender = sched.schedule();
      auto h =
          std::make_unique<op_holder<Op>>(sender, counting_recv{&completed});
      bexec::start(h->op);
      ops.push_back(std::move(h));
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ctx->stop();

  poster.join();
  worker.join();

  EXPECT_EQ(static_cast<int>(ops.size()), completed.load());
}

// Test 2: timer async_wait() operations aborted by stop.
// All 50 timer operations must complete.
TEST(LifecycleTest, stop_completes_all_timer_ops) {
  auto ctx = std::make_unique<bnio::io_context>();
  if (!ctx->is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  constexpr int N = 50;
  std::atomic<int> completed{0};
  std::vector<std::unique_ptr<op_holder_base>> ops;
  std::vector<bnio::steady_timer> timers;

  for (std::size_t i = 0; i < static_cast<std::size_t>(N); ++i) {
    timers.emplace_back(*ctx);
  }

  std::thread worker([&ctx]() { ctx->run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto far = std::chrono::steady_clock::now() + std::chrono::hours(24);
  for (std::size_t i = 0; i < static_cast<std::size_t>(N); ++i) {
    (void)timers[i].expires_at(far);
    auto sender = timers[i].async_wait();
    using Op = decltype(bexec::connect(sender, timer_recv{nullptr}));
    auto h = std::make_unique<op_holder<Op>>(sender, timer_recv{&completed});
    bexec::start(h->op);
    ops.push_back(std::move(h));
  }

  ctx->stop();

  worker.join();

  EXPECT_EQ(N, completed.load());
}

}  // namespace
