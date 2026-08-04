#include <gtest/gtest.h>

#include <bnio/bnio.h>

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
      : op(bexec::connect(std::forward<Sender>(s),
                          std::forward<Receiver>(r))) {}
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

struct probe_recv {
  std::atomic<bool>* done = nullptr;
  void set_value(std::error_code) noexcept {
    if (done) done->store(true, std::memory_order_release);
  }
  void set_stopped() noexcept {
    if (done) done->store(true, std::memory_order_release);
  }
};

// Issue 1 (stranded operations) under load. Multiple submitters publish
// schedule() operations concurrently, but the publish is gated so that it
// lands AFTER the last worker has finished its final drain and exited.
// Every started operation must still complete: on the lock-free baseline
// publish_cpu() enqueues into a shared queue no worker ever drains again
// (strand); on the fix publish_cpu() observes the stopping state under the
// submit lock, rejects the enqueue, and the caller completes inline.
TEST(ShutdownSubmitStressTest, concurrent_schedule_after_stop_all_complete) {
  constexpr int kRounds = 50;
  constexpr int kPosters = 8;
  constexpr int kOpsPerPoster = 1000;

  int rounds_with_strands = 0;

  for (int round = 0; round < kRounds; ++round) {
    auto ctx = std::make_unique<bnio::io_context>();
    if (!ctx->is_open()) {
      GTEST_SKIP() << "native I/O context is unavailable";
    }

    std::atomic<int> completed{0};
    std::atomic<int> posted{0};
    std::atomic<bool> worker_entered{false};
    std::atomic<int> posters_started{0};
    std::atomic<bool> g_go{false};
    std::vector<std::vector<std::unique_ptr<op_holder_base>>> poster_ops(
        static_cast<std::size_t>(kPosters));

    std::thread worker([&ctx]() { ctx->run(); });

    // Prove the worker entered run() by round-tripping one probe op.
    {
      auto probe = bexec::connect(ctx->get_post_scheduler().schedule(),
                                  probe_recv{&worker_entered});
      bexec::start(probe);
      while (!worker_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }

    std::vector<std::thread> posters;
    for (int p = 0; p < kPosters; ++p) {
      posters.emplace_back([&, p]() {
        auto scheduler = ctx->get_post_scheduler();
        using Sender = decltype(scheduler.schedule());
        using Op = decltype(bexec::connect(std::declval<Sender>(),
                                            counting_recv{nullptr}));
        auto& ops = poster_ops[static_cast<std::size_t>(p)];
        ops.reserve(static_cast<std::size_t>(kOpsPerPoster));
        // Announce readiness, then park until the worker has fully exited.
        // Every publish therefore lands after the last worker's final drain
        // — the Issue-1 window — with 8 posters hammering publish_cpu()
        // concurrently.
        posters_started.fetch_add(1, std::memory_order_release);
        while (!g_go.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        for (int i = 0; i < kOpsPerPoster; ++i) {
          auto sender = scheduler.schedule();
          auto h = std::make_unique<op_holder<Op>>(
              sender, counting_recv{&completed});
          bexec::start(h->op);
          ops.push_back(std::move(h));
        }
        posted.fetch_add(kOpsPerPoster, std::memory_order_relaxed);
      });
    }

    // Stop the context and let the worker drain and exit completely before
    // any poster publishes.
    while (posters_started.load(std::memory_order_acquire) != kPosters) {
      std::this_thread::yield();
    }
    (void)ctx->stop();
    worker.join();
    g_go.store(true, std::memory_order_release);

    for (auto& t : posters) {
      t.join();
    }

    // Every started operation must have completed. Bounded wait: stranded
    // operations never complete and would leave completed < posted.
    const int expected = posted.load(std::memory_order_acquire);
    for (int i = 0;
         i < 200 && completed.load(std::memory_order_acquire) < expected;
         ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (completed.load(std::memory_order_acquire) != expected) {
      ++rounds_with_strands;
    }
    poster_ops.clear();
  }

  EXPECT_EQ(rounds_with_strands, 0);
}

}  // namespace
