// Stress regression for the two-phase stop transition: concurrent
// request_stop(), stop(), join(), and publication against a live worker.
// Invariants per round:
//   - join() completes;
//   - every started operation reaches exactly one terminal receiver call;
//   - no receiver completes via set_stopped().
//
// A stranded operation or a double delivery fails the round; a lost
// transition is bounded by the CMake TIMEOUT and by the per-round wait.

#include <bnio/bnio.h>
#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct counts {
  std::atomic<int> completed{0};
  std::atomic<int> canceled{0};
  std::atomic<int> stopped{0};
};

struct schedule_recv {
  counts* c;
  void set_value(std::error_code ec) noexcept {
    c->completed.fetch_add(1, std::memory_order_relaxed);
    if (ec == std::make_error_code(std::errc::operation_canceled)) {
      c->canceled.fetch_add(1, std::memory_order_relaxed);
    }
  }
  void set_stopped() noexcept {
    c->completed.fetch_add(1, std::memory_order_relaxed);
    c->stopped.fetch_add(1, std::memory_order_relaxed);
  }
};

struct join_recv {
  std::atomic<bool>* done;
  void set_value() noexcept { done->store(true, std::memory_order_release); }
};

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

TEST(StopTransitionRaceStressTest, concurrent_request_stop_stop_join_submit) {
  constexpr int kRounds = 50;
  constexpr int kPublishers = 2;
  constexpr int kOpsPerPublisher = 16;
  constexpr int kOperations = kPublishers * kOpsPerPublisher;

  for (int round = 0; round < kRounds; ++round) {
    bnio::io_context ctx;
    counts c;

    auto scheduler = ctx.get_post_scheduler();
    std::vector<std::unique_ptr<op_holder_base>> holders;
    holders.reserve(kOperations);
    for (int i = 0; i < kOperations; ++i) {
      auto sender = scheduler.schedule();
      using Op = decltype(bexec::connect(sender, schedule_recv{nullptr}));
      holders.push_back(
          std::make_unique<op_holder<Op>>(sender, schedule_recv{&c}));
    }

    std::thread worker([&ctx] { (void)ctx.run(); });
    std::this_thread::sleep_for(2ms);

    std::atomic<int> started{0};
    std::atomic<bool> joined{false};
    std::barrier start_gate(kPublishers + 3);
    std::vector<std::thread> threads;
    threads.reserve(kPublishers + 3);

    for (int p = 0; p < kPublishers; ++p) {
      threads.emplace_back([&, p] {
        start_gate.arrive_and_wait();
        const int first = p * kOpsPerPublisher;
        const int last = first + kOpsPerPublisher;
        for (int i = first; i < last; ++i) {
          using Op = decltype(bexec::connect(
              std::declval<decltype(scheduler.schedule())>(),
              schedule_recv{nullptr}));
          bexec::start(static_cast<op_holder<Op>*>(
                           holders[static_cast<std::size_t>(i)].get())
                           ->op);
          started.fetch_add(1, std::memory_order_relaxed);
          std::this_thread::yield();
        }
      });
    }
    threads.emplace_back([&ctx, &start_gate] {
      start_gate.arrive_and_wait();
      for (int i = 0; i < 64; ++i) {
        ctx.request_stop();
        std::this_thread::yield();
      }
    });
    threads.emplace_back([&ctx, &start_gate] {
      start_gate.arrive_and_wait();
      (void)ctx.stop();
    });
    threads.emplace_back([&ctx, &joined, &start_gate] {
      start_gate.arrive_and_wait();
      auto join_sender = ctx.join();
      auto join_op = bexec::connect(join_sender, join_recv{&joined});
      bexec::start(join_op);
    });

    for (std::thread& t : threads) {
      t.join();
    }
    worker.join();

    ASSERT_TRUE(joined.load(std::memory_order_acquire))
        << "round " << round << ": join() did not complete";
    ASSERT_EQ(started.load(std::memory_order_relaxed), kOperations)
        << "round " << round << ": operations were not published";

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (c.completed.load(std::memory_order_relaxed) != kOperations &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(1ms);
    }

    ASSERT_EQ(c.completed.load(std::memory_order_relaxed), kOperations)
        << "round " << round
        << ": an operation was stranded or delivered "
           "more than once";
    ASSERT_EQ(c.stopped.load(std::memory_order_relaxed), 0)
        << "round " << round
        << ": set_stopped() is not a valid terminal "
           "state here";
    ASSERT_LE(c.canceled.load(std::memory_order_relaxed), kOperations)
        << "round " << round << ": more canceled completions than operations";
  }
}

}  // namespace
