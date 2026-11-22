// Concurrency regression for the two-phase stop transition: request_stop(),
// stop(), join(), and work publication race round after round. Every started
// operation must reach exactly one terminal receiver call, and set_stopped()
// is never a valid terminal state for these token-less receivers.
//
// The body runs in a forked child because a lost transition or a wrong
// finish gate deadlocks; the parent's watchdog turns that into a clean test
// failure.

#include <bnio/bnio.h>
#include <gtest/gtest.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <csignal>
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

[[nodiscard]] bool run_isolated(void (*body)()) {
  const pid_t pid = ::fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    body();
    ::_exit(0);
  }

  const auto deadline = std::chrono::steady_clock::now() + 15s;
  int status = 0;
  for (;;) {
    const pid_t result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    if (result < 0) {
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      (void)::kill(pid, SIGKILL);
      (void)::waitpid(pid, &status, 0);
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
}

void stop_transition_race_body() {
  constexpr int kRounds = 16;
  constexpr int kPublishers = 3;
  constexpr int kOpsPerPublisher = 32;
  constexpr int kOperations = kPublishers * kOpsPerPublisher;

  for (int round = 0; round < kRounds; ++round) {
    bnio::io_context ctx;
    counts c;

    // Pre-connect all operations so publishers only call start().
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
    std::this_thread::sleep_for(5ms);

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
      ctx.request_stop();
      ctx.request_stop();
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

    if (!joined.load(std::memory_order_acquire)) {
      ::_exit(1);
    }

    // Every operation that started must have exactly one terminal call.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (c.completed.load(std::memory_order_relaxed) <
               started.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(1ms);
    }

    if (started.load(std::memory_order_relaxed) != kOperations) {
      ::_exit(2);
    }
    if (c.completed.load(std::memory_order_relaxed) != kOperations) {
      ::_exit(3);
    }
    if (c.stopped.load(std::memory_order_relaxed) != 0) {
      ::_exit(4);
    }
  }
  ::_exit(0);
}

TEST(StopTransitionRaceTest, concurrent_request_stop_stop_join_publish) {
  EXPECT_TRUE(run_isolated(stop_transition_race_body));
}

}  // namespace
