// Regression test for the v0.1 circuit-break defect: a stop request from a
// signal handler running on the worker thread must unblock a run loop that
// is parked in the kernel wait with in-flight I/O and no timers armed.
//
// Historical defect and fix:
//   1. stop() used to wait for workers and excluded the calling worker from
//      the wake set (is_in_context() self-exclusion), so a signal-handler
//      stop() could both skip the wake and deadlock. stop() is now a
//      non-blocking request, and request_stop() is its zero-lock phase-1
//      subset: it only records the request and broadcasts the wake channel.
//   2. io_uring_context::wait_for_cqe_event() retried on -EINTR internally,
//      so the run loop never re-evaluated should_finish() after a signal.
//
// The handler therefore calls request_stop() — the only context operation
// valid from an async signal handler. The exiting worker or join() drives
// phase 2 (abort timers, publish stopping, drain).
//
// The scenario runs in a forked child so a regression hangs the child, not
// the test process; the parent enforces a watchdog timeout and reports a
// clean failure instead of stalling the test runner.

#include <bnio/bnio.h>
#include <gtest/gtest.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <bexec/bexec.hpp>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>

namespace {

// Child-side state: set before the handler is installed, read only from the
// handler afterwards.
bnio::io_context* g_child_ctx = nullptr;

extern "C" void on_sigusr1(int) noexcept {
  if (g_child_ctx != nullptr) {
    g_child_ctx->request_stop();
  }
}

struct op_holder_base {
  virtual ~op_holder_base() = default;
};

template <typename Op>
struct op_holder : op_holder_base {
  template <typename Sender, typename Receiver>
  op_holder(Sender&& s, Receiver&& r)
      : op(bexec::connect(std::forward<Sender>(s), std::forward<Receiver>(r))) {
  }
  Op op;
};

// Records how the pending accept completed: 1 = set_value(operation_canceled),
// 2 = set_stopped, 4 = set_value with an unexpected error code.
struct accept_sink {
  std::atomic<int>* outcome;
  void set_value(std::error_code ec, bnio::tcp_socket) noexcept {
    // Contract: context stop aborting inflight I/O completes via
    // set_value(operation_canceled).
    const int result =
        ec == std::make_error_code(std::errc::operation_canceled) ? 1 : 4;
    outcome->store(result, std::memory_order_relaxed);
  }
  void set_stopped() noexcept { outcome->store(2, std::memory_order_relaxed); }
};

// Same classification for a pending timer wait.
struct timer_sink {
  std::atomic<int>* outcome;
  void set_value(std::error_code ec) noexcept {
    const int result =
        ec == std::make_error_code(std::errc::operation_canceled) ? 1 : 4;
    outcome->store(result, std::memory_order_relaxed);
  }
  void set_stopped() noexcept { outcome->store(2, std::memory_order_relaxed); }
};

// arm_timer: also stage one long timer wait, so phase 2 must abort it and
// deliver set_value(operation_canceled) after request_stop().
[[noreturn]] void child_main(bool arm_timer) noexcept {
  bnio::io_context ctx;
  if (!ctx.is_open()) {
    _exit(3);
  }

  bnio::tcp_acceptor acp;
  std::error_code ec;
  if ((ec = acp.open(bnio::ip::tcp::v4())) ||
      (ec = acp.bind(bnio::ip::endpoint(bnio::ip::address::any_v4(), 0))) ||
      (ec = acp.listen(16))) {
    _exit(3);
  }

  // Arm one accept that never completes on its own: it keeps the run loop
  // in the no-timeout kernel wait with in-flight I/O, exactly like an idle
  // echo server.
  std::atomic<int> accept_outcome{0};
  using accept_sender_t =
      decltype(acp.async_accept(ctx.get_post_scheduler(), 0));
  using accept_op_t = decltype(bexec::connect(std::declval<accept_sender_t>(),
                                              accept_sink{nullptr}));
  auto holder = std::make_unique<op_holder<accept_op_t>>(
      acp.async_accept(ctx.get_post_scheduler(), 0),
      accept_sink{&accept_outcome});
  bexec::start(holder->op);

  std::atomic<int> timer_outcome{0};
  std::unique_ptr<bnio::steady_timer> timer;
  std::unique_ptr<op_holder_base> timer_holder;
  if (arm_timer) {
    timer = std::make_unique<bnio::steady_timer>(ctx);
    if (timer->expires_at(std::chrono::steady_clock::now() +
                          std::chrono::hours(1)) != 0U) {
      _exit(3);
    }
    auto timer_sender = timer->async_wait();
    using timer_op_t = decltype(bexec::connect(
        std::declval<decltype(timer_sender)>(), timer_sink{}));
    auto op = std::make_unique<op_holder<timer_op_t>>(
        timer_sender, timer_sink{&timer_outcome});
    bexec::start(op->op);
    timer_holder = std::move(op);
  }

  g_child_ctx = &ctx;
  struct sigaction sa{};
  sa.sa_handler = &on_sigusr1;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (::sigaction(SIGUSR1, &sa, nullptr) != 0) {
    _exit(3);
  }

  // Deliver SIGUSR1 to the worker (main) thread after it had ample time to
  // park inside the kernel wait.
  const pthread_t worker_tid = pthread_self();
  std::thread signaler([worker_tid] {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    (void)::pthread_kill(worker_tid, SIGUSR1);
  });
  signaler.detach();

  (void)ctx.run();
  // run() returned: phase 2 must have aborted the pending accept (and, when
  // armed, the long timer), delivering set_value(operation_canceled).
  const bool accept_canceled =
      accept_outcome.load(std::memory_order_relaxed) == 1;
  const bool timer_canceled =
      !arm_timer || timer_outcome.load(std::memory_order_relaxed) == 1;
  _exit(accept_canceled && timer_canceled ? 0 : 2);
}

[[nodiscard]] int run_child_and_watch(bool arm_timer, const char* failure) {
  const pid_t pid = ::fork();
  EXPECT_NE(pid, -1) << "fork failed: " << std::strerror(errno);
  if (pid == -1) {
    return -1;
  }
  if (pid == 0) {
    child_main(arm_timer);
  }

  // Watchdog: the child must exit well within this budget. A lost-wakeup
  // regression leaves it parked in io_uring_enter forever.
  constexpr auto budget = std::chrono::seconds(8);
  const auto deadline = std::chrono::steady_clock::now() + budget;
  int status = 0;
  pid_t reaped = 0;
  for (;;) {
    reaped = ::waitpid(pid, &status, WNOHANG);
    if (reaped != 0 || std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (reaped == 0) {
    (void)::kill(pid, SIGKILL);
    (void)::waitpid(pid, &status, 0);
    ADD_FAILURE() << failure;
    return -1;
  }
  EXPECT_EQ(reaped, pid) << "waitpid failed: " << std::strerror(errno);
  if (!WIFEXITED(status)) {
    ADD_FAILURE() << "child terminated abnormally (status=0x" << std::hex
                  << status << ")";
    return -1;
  }
  return WEXITSTATUS(status);
}

TEST(LifecycleTest, request_stop_from_signal_handler_unblocks_kernel_wait) {
  if (!bnio::io_context().is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  const int exit_status = run_child_and_watch(
      /*arm_timer=*/false,
      "run() did not return after request_stop() from a signal handler; the "
      "worker stayed parked in the kernel wait (lost wakeup)");
  if (exit_status < 0) {
    return;
  }
  EXPECT_EQ(exit_status, 0)
      << "child exit " << exit_status
      << " (2 = run() returned but the pending accept did not complete via "
         "set_value(operation_canceled); 3 = child setup failed)";
}

TEST(LifecycleTest, request_stop_from_signal_handler_cancels_armed_timer) {
  if (!bnio::io_context().is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  const int exit_status = run_child_and_watch(
      /*arm_timer=*/true,
      "run() did not return after request_stop() from a signal handler with "
      "an armed long timer; the worker stayed parked in the kernel wait");
  if (exit_status < 0) {
    return;
  }
  EXPECT_EQ(exit_status, 0)
      << "child exit " << exit_status
      << " (2 = run() returned but the pending accept and/or long timer did "
         "not complete via set_value(operation_canceled); 3 = child setup "
         "failed)";
}

}  // namespace
