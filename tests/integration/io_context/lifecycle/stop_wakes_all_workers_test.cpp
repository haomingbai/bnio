// Regression tests for the stop() wake contract: once stop() has been
// requested, every thread inside io_context::run() must leave the native
// poller, so the application can join those threads with std::thread::join
// (never io_context::join()). The old macOS/kqueue path could leave peers
// parked in kevent() with no timeout, hanging that join; each scenario runs
// in a forked child guarded by a watchdog so the hang fails the test instead
// of parking the runner. Each scenario repeats kRounds times because one
// round can pass even without the directed stop broadcast: the regression
// shows only when a peer is still parked after the shared wake is drained.

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

// More peers widen the window a missed directed wake can strand one of them,
// and one round can be won by the shared hint alone; repetition plus a wider
// parked set keeps a silent pass unlikely.
constexpr int kWorkerCount = 8;
constexpr int kRounds = 16;

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

constexpr auto kChildTimeout = 25s;
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

void start_workers(bnio::io_context& ctx, std::vector<std::thread>& workers) {
  workers.reserve(kWorkerCount);
  for (int i = 0; i < kWorkerCount; ++i) {
    workers.emplace_back([&ctx] { (void)ctx.run(); });
  }
}

// A worker that missed the stop wake keeps its join pending; the watchdog
// turns that hang into a failed child exit.
void join_workers(std::vector<std::thread>& workers) {
  for (auto& worker : workers) {
    worker.join();
  }
}

// The 200ms delay leaves the peer workers parked in the native poller while
// this callback stops the context.
struct stopping_receiver {
  std::atomic<bool>* entered;
  bnio::io_context* ctx;

  void set_value(std::error_code) noexcept {
    entered->store(true, std::memory_order_release);
    std::this_thread::sleep_for(200ms);
    (void)ctx->stop();
  }

  void set_stopped() noexcept { set_value(std::error_code{}); }
};

struct completion_receiver {
  std::atomic<bool>* completed;

  void set_value(std::error_code) noexcept {
    completed->store(true, std::memory_order_release);
  }

  void set_stopped() noexcept { set_value(std::error_code{}); }
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

// A: stop() runs on a worker callback; the joining thread is the test main
// thread and must still wait only for workers that observed the stop.
void worker_callback_stop_body() {
  for (int round = 0; round < kRounds; ++round) {
    bnio::io_context ctx;
    std::atomic<bool> entered{false};

    std::vector<std::thread> workers;
    start_workers(ctx, workers);
    // Assumption: the workers reach the native poller well within this delay.
    std::this_thread::sleep_for(200ms);

    auto scheduler = ctx.get_post_scheduler();
    auto sender = scheduler.schedule();
    using op_type =
        decltype(bexec::connect(sender, stopping_receiver{nullptr, nullptr}));
    auto operation = std::make_unique<op_holder<op_type>>(
        sender, stopping_receiver{&entered, &ctx});
    bexec::start(operation->op);

    if (!wait_until([&] { return entered.load(std::memory_order_acquire); })) {
      ::_exit(1);
    }

    join_workers(workers);
  }
  ::_exit(0);
}

// B: stop() from outside every worker. The worker that consumes the shared
// wake publishes Phase 2 and must wake the ones still parked.
void external_stop_body() {
  for (int round = 0; round < kRounds; ++round) {
    bnio::io_context ctx;
    std::atomic<bool> completed{false};

    std::vector<std::thread> workers;
    start_workers(ctx, workers);

    auto scheduler = ctx.get_post_scheduler();
    auto sender = scheduler.schedule();
    using op_type =
        decltype(bexec::connect(sender, completion_receiver{nullptr}));
    auto operation = std::make_unique<op_holder<op_type>>(
        sender, completion_receiver{&completed});
    bexec::start(operation->op);

    // Prove the loop is live before stopping, so an early-exiting worker
    // cannot mask the wake path; the delay then lets every worker park.
    if (!wait_until(
            [&] { return completed.load(std::memory_order_acquire); })) {
      ::_exit(1);
    }
    std::this_thread::sleep_for(300ms);

    (void)ctx.stop();
    join_workers(workers);
  }
  ::_exit(0);
}

}  // namespace

TEST(LifecycleStopWakesAllWorkersTest, worker_callback_stop) {
  EXPECT_TRUE(run_isolated(worker_callback_stop_body));
}

TEST(LifecycleStopWakesAllWorkersTest, external_stop) {
  EXPECT_TRUE(run_isolated(external_stop_body));
}
