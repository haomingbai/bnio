// Regression tests for the non-blocking stop() contract:
//   - stop() returns without waiting for workers;
//   - stop() called from a receiver callback returns immediately (join() from
//     such a callback is unsupported and is not exercised here);
//   - the no-worker stop fallback and the join barrier each drain the shared
//     queues exactly once.
//
// Every deadlock-prone scenario runs in a forked child with a watchdog so a
// regression fails the test instead of parking the test runner.

#include <bnio/bnio.h>
#include <gtest/gtest.h>
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

struct op_holder_base {
  virtual ~op_holder_base() = default;
};

template <typename Op>
struct op_holder : op_holder_base {
  Op op;
  template <typename Sender, typename Receiver>
  op_holder(Sender&& sender, Receiver&& receiver)
      : op(bexec::connect(std::forward<Sender>(sender),
                          std::forward<Receiver>(receiver))) {}
};

constexpr auto kChildTimeout = 10s;
constexpr auto kStepTimeout = 2s;

template <class Pred>
bool wait_until(Pred pred, std::chrono::milliseconds limit = kStepTimeout) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

// Worker callback: reports entry, optionally calls stop() after the barrier.
struct gate_receiver {
  std::atomic<bool>* entered;
  std::atomic<bool>* go;
  std::atomic<bool>* done;
  bnio::io_context* ctx;
  bool stop_inside;

  void set_value(std::error_code) noexcept {
    entered->store(true, std::memory_order_release);
    while (!go->load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    if (stop_inside) {
      (void)ctx->stop();
    }
    done->store(true, std::memory_order_release);
  }

  void set_stopped() noexcept { set_value(std::error_code{}); }
};

// Two workers rendezvous, then both call stop() from receiver callbacks.
struct barrier_receiver {
  std::atomic<int>* entered;
  std::atomic<int>* done;
  std::atomic<bool>* go;
  bnio::io_context* ctx;

  void set_value(std::error_code) noexcept {
    entered->fetch_add(1, std::memory_order_acq_rel);
    while (!go->load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    (void)ctx->stop();
    done->fetch_add(1, std::memory_order_acq_rel);
  }

  void set_stopped() noexcept { set_value(std::error_code{}); }
};

// Runs inside stop()'s own shared-queue drain and re-enters stop().
struct reentrant_receiver {
  bnio::io_context* ctx;
  std::atomic<bool>* done;

  void set_value(std::error_code) noexcept {
    (void)ctx->stop();
    done->store(true, std::memory_order_release);
  }

  void set_stopped() noexcept { set_value(std::error_code{}); }
};

struct counting_receiver {
  std::atomic<int>* completed;
  void set_value(std::error_code) noexcept {
    completed->fetch_add(1, std::memory_order_acq_rel);
  }
  void set_stopped() noexcept {
    completed->fetch_add(1, std::memory_order_acq_rel);
  }
};

struct join_receiver {
  void set_value() noexcept {}
};

// Runs `body` in a child process and reports whether it exited 0 within the
// watchdog window. The child must use _exit() so no live-thread destructor
// runs on failure.
[[nodiscard]] bool run_isolated(void (*body)()) {
  const pid_t pid = ::fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    body();
    ::_exit(0);
  }

  const auto deadline = std::chrono::steady_clock::now() + kChildTimeout;
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

// A: an external stop() wins while a worker callback then calls stop().
void external_stop_and_worker_stop_body() {
  bnio::io_context ctx;
  std::atomic<bool> entered{false};
  std::atomic<bool> go{false};
  std::atomic<bool> done{false};

  auto scheduler = ctx.get_post_scheduler();
  auto sender = scheduler.schedule();
  auto operation = bexec::connect(
      std::move(sender), gate_receiver{&entered, &go, &done, &ctx, true});
  bexec::start(operation);

  std::thread worker([&ctx] { (void)ctx.run(); });
  if (!wait_until([&] { return entered.load(std::memory_order_acquire); })) {
    ::_exit(1);
  }

  std::thread stopper([&ctx] { (void)ctx.stop(); });
  if (!wait_until([&] { return ctx.is_stopped(); })) {
    ::_exit(1);
  }

  go.store(true, std::memory_order_release);
  if (!wait_until([&] { return done.load(std::memory_order_acquire); })) {
    ::_exit(1);
  }
  // Do not join: the child exits below; a still-running run() is harmless.
  ::_exit(0);
}

// B: two workers both call stop() from receiver callbacks.
void two_worker_callbacks_stop_body() {
  bnio::io_context ctx;
  std::atomic<int> entered{0};
  std::atomic<int> done{0};
  std::atomic<bool> go{false};

  auto scheduler = ctx.get_post_scheduler();

  std::thread worker1([&ctx] { (void)ctx.run(); });
  auto sender1 = scheduler.schedule();
  auto op1 = bexec::connect(std::move(sender1),
                            barrier_receiver{&entered, &done, &go, &ctx});
  bexec::start(op1);
  if (!wait_until([&] { return entered.load() >= 1; })) {
    ::_exit(1);
  }

  std::thread worker2([&ctx] { (void)ctx.run(); });
  auto sender2 = scheduler.schedule();
  auto op2 = bexec::connect(std::move(sender2),
                            barrier_receiver{&entered, &done, &go, &ctx});
  bexec::start(op2);
  if (!wait_until([&] { return entered.load() >= 2; })) {
    ::_exit(1);
  }

  go.store(true, std::memory_order_release);
  if (!wait_until([&] { return done.load() >= 2; })) {
    ::_exit(1);
  }
  ::_exit(0);
}

// C: the stop() no-worker fallback delivers a queued receiver that calls
// stop() again on the same thread.
void reentrant_stop_in_stop_drain_body() {
  bnio::io_context ctx;
  std::atomic<bool> done{false};

  auto scheduler = ctx.get_post_scheduler();
  auto sender = scheduler.schedule();
  auto operation =
      bexec::connect(std::move(sender), reentrant_receiver{&ctx, &done});
  bexec::start(operation);

  std::thread stopper([&ctx] { (void)ctx.stop(); });
  if (!wait_until([&] { return done.load(std::memory_order_acquire); })) {
    ::_exit(1);
  }
  ::_exit(0);
}

// D: stop() returns while the worker is still inside a receiver callback.
void stop_is_nonblocking_body() {
  bnio::io_context ctx;
  std::atomic<bool> entered{false};
  std::atomic<bool> go{false};
  std::atomic<bool> done{false};

  auto scheduler = ctx.get_post_scheduler();
  auto sender = scheduler.schedule();
  auto operation = bexec::connect(
      std::move(sender), gate_receiver{&entered, &go, &done, &ctx, false});
  bexec::start(operation);

  std::thread worker([&ctx] { (void)ctx.run(); });
  if (!wait_until([&] { return entered.load(std::memory_order_acquire); })) {
    ::_exit(1);
  }

  std::atomic<bool> stopped{false};
  std::thread stopper([&] {
    (void)ctx.stop();
    stopped.store(true, std::memory_order_release);
  });

  // The worker callback is still blocked on `go`; stop() must return anyway.
  if (!wait_until([&] { return stopped.load(std::memory_order_acquire); },
                  1s)) {
    ::_exit(1);
  }
  if (done.load(std::memory_order_acquire)) {
    ::_exit(1);
  }

  go.store(true, std::memory_order_release);
  if (!wait_until([&] { return done.load(std::memory_order_acquire); })) {
    ::_exit(1);
  }
  ::_exit(0);
}

// E: join() is the barrier that drains shared work when no worker ran.
void join_drains_shared_queues_body() {
  bnio::io_context ctx;
  constexpr int kOps = 64;
  std::atomic<int> completed{0};

  auto scheduler = ctx.get_post_scheduler();
  std::vector<std::unique_ptr<op_holder_base>> ops;
  ops.reserve(kOps);
  for (int i = 0; i < kOps; ++i) {
    auto sender = scheduler.schedule();
    using Op = decltype(bexec::connect(sender, counting_receiver{nullptr}));
    auto holder =
        std::make_unique<op_holder<Op>>(sender, counting_receiver{&completed});
    bexec::start(holder->op);
    ops.push_back(std::move(holder));
  }

  auto join_sender = ctx.join();
  auto join_op = bexec::connect(join_sender, join_receiver{});
  bexec::start(join_op);

  if (completed.load(std::memory_order_acquire) != kOps) {
    ::_exit(1);
  }
  ::_exit(0);
}

}  // namespace

TEST(LifecycleStopNonblockingTest, external_stop_and_worker_stop) {
  EXPECT_TRUE(run_isolated(external_stop_and_worker_stop_body));
}

TEST(LifecycleStopNonblockingTest, two_worker_callbacks_stop) {
  EXPECT_TRUE(run_isolated(two_worker_callbacks_stop_body));
}

TEST(LifecycleStopNonblockingTest, reentrant_stop_in_stop_drain) {
  EXPECT_TRUE(run_isolated(reentrant_stop_in_stop_drain_body));
}

TEST(LifecycleStopNonblockingTest, stop_returns_before_callback_returns) {
  EXPECT_TRUE(run_isolated(stop_is_nonblocking_body));
}

TEST(LifecycleStopNonblockingTest, join_drains_shared_queues_without_worker) {
  EXPECT_TRUE(run_isolated(join_drains_shared_queues_body));
}
