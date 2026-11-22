// Join/stop interaction regressions for the two-phase stop:
//   - join() after request_stop() alone completes phase 2 and drains;
//   - concurrent join() and stop() callers all return;
//   - stop() from a worker while another thread waits in join() does not
//     deadlock;
//   - join() never spins waiting for some other caller to publish the
//     terminal state (it completes phase 2 itself).
//
// Deadlock-prone scenarios run in a forked child with a watchdog; a failure
// reports as a clean gtest failure instead of parking the test runner.

#include <bnio/bnio.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

template <class Pred>
bool wait_until(Pred pred, std::chrono::milliseconds limit = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

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

// Worker callback that rendezvouses with the main thread, then calls stop()
// from the worker just as another thread is waiting inside join().
struct worker_stop_recv {
  bnio::io_context* ctx;
  std::atomic<bool>* entered;
  std::atomic<bool>* go;
  std::atomic<bool>* done;
  std::atomic<int>* stop_ret;

  void set_value(std::error_code) noexcept {
    entered->store(true, std::memory_order_release);
    while (!go->load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    stop_ret->store(ctx->stop(), std::memory_order_release);
    done->store(true, std::memory_order_release);
  }
  void set_stopped() noexcept { set_value(std::error_code{}); }
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

TEST(JoinStopRaceTest, join_after_request_only_completes_and_drains) {
  bnio::io_context ctx;
  constexpr int kOps = 16;
  counts c;

  auto scheduler = ctx.get_post_scheduler();
  std::vector<std::unique_ptr<op_holder_base>> holders;
  holders.reserve(kOps);
  for (int i = 0; i < kOps; ++i) {
    auto sender = scheduler.schedule();
    using Op = decltype(bexec::connect(sender, schedule_recv{nullptr}));
    holders.push_back(
        std::make_unique<op_holder<Op>>(sender, schedule_recv{&c}));
    bexec::start(static_cast<op_holder<Op>*>(holders.back().get())->op);
  }

  ctx.request_stop();
  ASSERT_TRUE(ctx.is_stopped());

  std::atomic<bool> joined{false};
  auto join_sender = ctx.join();
  auto join_op = bexec::connect(join_sender, join_recv{&joined});
  bexec::start(join_op);

  EXPECT_TRUE(joined.load(std::memory_order_acquire));
  EXPECT_EQ(c.completed.load(std::memory_order_relaxed), kOps);
  EXPECT_EQ(c.canceled.load(std::memory_order_relaxed), kOps);
  EXPECT_EQ(c.stopped.load(std::memory_order_relaxed), 0);
}

// Four joiners and two stop() callers race while one worker runs. Every
// caller must return.
void concurrent_join_and_stop_body() {
  bnio::io_context ctx;
  std::thread worker([&ctx] { (void)ctx.run(); });
  std::this_thread::sleep_for(100ms);

  constexpr int kJoiners = 4;
  std::vector<std::atomic<bool>> joined(kJoiners);
  for (std::atomic<bool>& flag : joined) {
    flag.store(false, std::memory_order_relaxed);
  }
  std::vector<std::thread> joiners;
  joiners.reserve(kJoiners);
  for (int i = 0; i < kJoiners; ++i) {
    joiners.emplace_back([&ctx, &joined, i] {
      auto join_sender = ctx.join();
      auto join_op = bexec::connect(
          join_sender, join_recv{&joined[static_cast<std::size_t>(i)]});
      bexec::start(join_op);
    });
  }

  std::atomic<int> stop_zero{0};
  std::thread stopper([&ctx, &stop_zero] {
    if (ctx.stop() == 0) {
      stop_zero.fetch_add(1, std::memory_order_relaxed);
    }
  });
  if (ctx.stop() == 0) {
    stop_zero.fetch_add(1, std::memory_order_relaxed);
  }

  stopper.join();
  for (std::thread& joiner : joiners) {
    joiner.join();
  }
  worker.join();

  if (stop_zero.load(std::memory_order_relaxed) != 2) {
    ::_exit(1);
  }
  for (const std::atomic<bool>& flag : joined) {
    if (!flag.load(std::memory_order_acquire)) {
      ::_exit(1);
    }
  }
  ::_exit(0);
}

TEST(JoinStopRaceTest, concurrent_join_and_stop_both_return) {
  EXPECT_TRUE(run_isolated(concurrent_join_and_stop_body));
}

// A worker callback calls stop() while another thread is already blocked in
// join(). join() must still observe the worker exiting and drain.
void stop_from_worker_while_join_waits_body() {
  bnio::io_context ctx;
  std::atomic<bool> entered{false};
  std::atomic<bool> go{false};
  std::atomic<bool> worker_stopped{false};
  std::atomic<int> stop_ret{-1};
  std::atomic<bool> joined{false};

  auto scheduler = ctx.get_post_scheduler();
  auto sender = scheduler.schedule();
  using Op = decltype(bexec::connect(
      sender, worker_stop_recv{nullptr, nullptr, nullptr, nullptr, nullptr}));
  auto operation = std::make_unique<op_holder<Op>>(
      sender,
      worker_stop_recv{&ctx, &entered, &go, &worker_stopped, &stop_ret});
  bexec::start(operation->op);

  std::thread worker([&ctx] { (void)ctx.run(); });
  if (!wait_until([&] { return entered.load(std::memory_order_acquire); })) {
    ::_exit(1);
  }

  std::thread joiner([&ctx, &joined] {
    auto join_sender = ctx.join();
    auto join_op = bexec::connect(join_sender, join_recv{&joined});
    bexec::start(join_op);
  });

  // Gate: join() has issued its stop request and is now waiting for the
  // worker. Only then does the worker call stop() from its callback.
  if (!wait_until([&] { return ctx.is_stopped(); })) {
    ::_exit(1);
  }
  go.store(true, std::memory_order_release);

  joiner.join();
  worker.join();

  if (!joined.load(std::memory_order_acquire)) {
    ::_exit(1);
  }
  if (!worker_stopped.load(std::memory_order_acquire)) {
    ::_exit(1);
  }
  if (stop_ret.load(std::memory_order_acquire) != 0) {
    ::_exit(1);
  }
  ::_exit(0);
}

TEST(JoinStopRaceTest, stop_from_worker_while_join_waits_does_not_deadlock) {
  EXPECT_TRUE(run_isolated(stop_from_worker_while_join_waits_body));
}

TEST(JoinStopRaceTest, join_does_not_spin_waiting_for_external_publication) {
  bnio::io_context ctx;
  counts c;

  auto scheduler = ctx.get_post_scheduler();
  std::vector<std::unique_ptr<op_holder_base>> holders;
  for (int i = 0; i < 8; ++i) {
    auto sender = scheduler.schedule();
    using Op = decltype(bexec::connect(sender, schedule_recv{nullptr}));
    holders.push_back(
        std::make_unique<op_holder<Op>>(sender, schedule_recv{&c}));
    bexec::start(static_cast<op_holder<Op>*>(holders.back().get())->op);
  }

  ctx.request_stop();

  // No stop() fallback and no external terminal-state publication: join()
  // itself must complete phase 2 and drain within a bounded budget.
  const auto start = std::chrono::steady_clock::now();
  std::atomic<bool> joined{false};
  auto join_sender = ctx.join();
  auto join_op = bexec::connect(join_sender, join_recv{&joined});
  bexec::start(join_op);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(joined.load(std::memory_order_acquire));
  EXPECT_LT(elapsed, 3s) << "join() spun waiting for an external publication";
  EXPECT_EQ(c.completed.load(std::memory_order_relaxed), 8);
  EXPECT_EQ(c.canceled.load(std::memory_order_relaxed), 8);
  EXPECT_EQ(c.stopped.load(std::memory_order_relaxed), 0);
}

}  // namespace
