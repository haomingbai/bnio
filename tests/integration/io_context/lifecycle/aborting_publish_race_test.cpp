// Race regression for publication during the Phase-2 "aborting" window.
//
// complete_stop() publishes stopping only after abort_pending_timer_waits()
// has walked the timer heap. Publishers that take submit_lock inside that
// window observe life_state == aborting and must be rejected (return false,
// complete inline); publishers that land before it are queued and drained by
// join(). Either way every started operation must reach exactly one terminal
// receiver call and set_stopped() is never valid for these token-less
// receivers.
//
// Many far-future timers widen the abort walk so publishers overlap it. The
// body runs in a forked child with a watchdog: a lost transition or a
// stranded operation fails cleanly instead of parking the test runner.

#include <bnio/bnio.h>
#include <gtest/gtest.h>
#include <poll.h>
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

// The race body runs in a forked child, so _exit() would otherwise discard
// the child's coverage counters. The target defines BNIO_TEST_COVERAGE only
// when it is instrumented with --coverage.
#if defined(BNIO_TEST_COVERAGE)
extern "C" void __gcov_dump(void);
#endif

namespace {

using namespace std::chrono_literals;

struct terminal_counts {
  std::atomic<int> value_ok{0};
  std::atomic<int> value_canceled{0};
  std::atomic<int> stopped{0};
  std::atomic<int> unexpected_error{0};

  void record(std::error_code ec) noexcept {
    if (!ec) {
      value_ok.fetch_add(1, std::memory_order_relaxed);
    } else if (ec == std::make_error_code(std::errc::operation_canceled)) {
      value_canceled.fetch_add(1, std::memory_order_relaxed);
    } else {
      unexpected_error.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

struct schedule_recv {
  terminal_counts* counts;

  void set_value(std::error_code ec) noexcept { counts->record(ec); }
  void set_stopped() noexcept {
    counts->stopped.fetch_add(1, std::memory_order_relaxed);
  }
};

struct poll_recv {
  terminal_counts* counts;

  void set_value(std::error_code ec, unsigned) noexcept { counts->record(ec); }
  void set_stopped() noexcept {
    counts->stopped.fetch_add(1, std::memory_order_relaxed);
  }
};

struct timer_recv {
  terminal_counts* counts;

  void set_value(std::error_code ec) noexcept { counts->record(ec); }
  void set_stopped() noexcept {
    counts->stopped.fetch_add(1, std::memory_order_relaxed);
  }
};

struct join_recv {
  std::atomic<bool>* done;
  void set_value() noexcept { done->store(true, std::memory_order_release); }
};

template <typename Op>
struct op_holder {
  Op op;
  template <typename Sender, typename Receiver>
  op_holder(Sender&& sender, Receiver&& receiver)
      : op(bexec::connect(std::forward<Sender>(sender),
                          std::forward<Receiver>(receiver))) {}
};

constexpr int kRounds = 6;
constexpr int kTimers = 512;
constexpr int kPublishers = 4;
constexpr int kScheduleOpsPerPublisher = 128;
constexpr int kPollOpsPerPublisher = 128;

[[nodiscard]] bool run_isolated(void (*body)()) {
  const pid_t pid = ::fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    body();
#if defined(BNIO_TEST_COVERAGE)
    __gcov_dump();
#endif
    ::_exit(0);
  }

  const auto deadline = std::chrono::steady_clock::now() + 20s;
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

void aborting_publish_body() {
  for (int round = 0; round < kRounds; ++round) {
    bnio::io_context ctx;
    if (!ctx.is_open()) {
      return;  // native backend unavailable: nothing to exercise
    }

    terminal_counts timer_counts;
    std::vector<terminal_counts> schedule_counts(kPublishers);
    std::vector<terminal_counts> poll_counts(kPublishers);

    auto scheduler = ctx.get_post_scheduler();
    using SchedSender = decltype(scheduler.schedule());
    using SchedOp = decltype(bexec::connect(std::declval<SchedSender>(),
                                            schedule_recv{nullptr}));
    using PollSender = decltype(bnio::async_poll(
        scheduler, bnio::async_io::descriptor_view(-1),
        static_cast<unsigned>(POLLIN)));
    using PollOp = decltype(bexec::connect(std::declval<PollSender>(),
                                           poll_recv{nullptr}));
    using TimerSender =
        decltype(std::declval<bnio::steady_timer&>().async_wait());
    using TimerOp = decltype(bexec::connect(std::declval<TimerSender>(),
                                            timer_recv{nullptr}));

    // Timers outlive their operations; all operations stay alive until after
    // join() drained/rejected every one.
    std::vector<std::unique_ptr<bnio::steady_timer>> timers;
    std::vector<std::unique_ptr<op_holder<TimerOp>>> timer_ops;
    std::vector<std::unique_ptr<op_holder<SchedOp>>> schedule_ops;
    std::vector<std::unique_ptr<op_holder<PollOp>>> poll_ops;
    timers.reserve(kTimers);
    timer_ops.reserve(kTimers);
    schedule_ops.reserve(kPublishers * kScheduleOpsPerPublisher);
    poll_ops.reserve(kPublishers * kPollOpsPerPublisher);

    // Far-future waits: complete_stop() must abort all of them while walking
    // the timer heap, which is exactly the window publishers race against.
    const auto far_deadline = std::chrono::steady_clock::now() + 24h;
    for (int i = 0; i < kTimers; ++i) {
      auto timer = std::make_unique<bnio::steady_timer>(ctx);
      if (timer->expires_at(far_deadline) != 0U) {
        ::_exit(1);
      }
      auto sender = timer->async_wait();
      auto operation = std::make_unique<op_holder<TimerOp>>(
          sender, timer_recv{&timer_counts});
      bexec::start(operation->op);
      timers.push_back(std::move(timer));
      timer_ops.push_back(std::move(operation));
    }

    // Pre-connect the racing work; publishers only call start() after the
    // gate so the whole burst overlaps join()'s Phase-2 transition.
    for (std::size_t p = 0; p < static_cast<std::size_t>(kPublishers); ++p) {
      for (int i = 0; i < kScheduleOpsPerPublisher; ++i) {
        auto sender = scheduler.schedule();
        schedule_ops.push_back(std::make_unique<op_holder<SchedOp>>(
            sender, schedule_recv{&schedule_counts[p]}));
      }
      for (int i = 0; i < kPollOpsPerPublisher; ++i) {
        auto sender =
            bnio::async_poll(scheduler, bnio::async_io::descriptor_view(-1),
                             static_cast<unsigned>(POLLIN));
        poll_ops.push_back(std::make_unique<op_holder<PollOp>>(
            sender, poll_recv{&poll_counts[p]}));
      }
    }

    std::barrier gate(kPublishers + 1);
    std::vector<std::thread> publishers;
    publishers.reserve(kPublishers);
    for (int p = 0; p < kPublishers; ++p) {
      publishers.emplace_back([&, p] {
        gate.arrive_and_wait();
        const int schedule_first = p * kScheduleOpsPerPublisher;
        const int poll_first = p * kPollOpsPerPublisher;
        for (int i = 0; i < kScheduleOpsPerPublisher; ++i) {
          bexec::start(
              schedule_ops[static_cast<std::size_t>(schedule_first + i)]->op);
        }
        for (int i = 0; i < kPollOpsPerPublisher; ++i) {
          bexec::start(poll_ops[static_cast<std::size_t>(poll_first + i)]->op);
        }
      });
    }

    gate.arrive_and_wait();
    std::atomic<bool> joined{false};
    auto join_sender = ctx.join();
    auto join_op = bexec::connect(join_sender, join_recv{&joined});
    bexec::start(join_op);
    if (!joined.load(std::memory_order_acquire)) {
      ::_exit(2);
    }
    for (std::thread& publisher : publishers) {
      publisher.join();
    }

    // The abort walk and the drain must cover every timer.
    if (timer_counts.value_canceled.load(std::memory_order_relaxed) !=
            kTimers ||
        timer_counts.value_ok.load(std::memory_order_relaxed) != 0 ||
        timer_counts.stopped.load(std::memory_order_relaxed) != 0 ||
        timer_counts.unexpected_error.load(std::memory_order_relaxed) != 0) {
      ::_exit(3);
    }

    for (std::size_t p = 0; p < static_cast<std::size_t>(kPublishers); ++p) {
      const int schedule_total =
          schedule_counts[p].value_ok.load(std::memory_order_relaxed) +
          schedule_counts[p].value_canceled.load(std::memory_order_relaxed) +
          schedule_counts[p].stopped.load(std::memory_order_relaxed) +
          schedule_counts[p].unexpected_error.load(std::memory_order_relaxed);
      const int poll_total =
          poll_counts[p].value_ok.load(std::memory_order_relaxed) +
          poll_counts[p].value_canceled.load(std::memory_order_relaxed) +
          poll_counts[p].stopped.load(std::memory_order_relaxed) +
          poll_counts[p].unexpected_error.load(std::memory_order_relaxed);

      // No worker ever ran, so no operation can complete successfully. Every
      // published operation completes through the stop channel exactly once.
      if (schedule_total != kScheduleOpsPerPublisher ||
          schedule_counts[p].value_canceled.load(std::memory_order_relaxed) !=
              kScheduleOpsPerPublisher ||
          schedule_counts[p].value_ok.load(std::memory_order_relaxed) != 0 ||
          schedule_counts[p].stopped.load(std::memory_order_relaxed) != 0 ||
          schedule_counts[p].unexpected_error.load(std::memory_order_relaxed) !=
              0) {
        ::_exit(4);
      }
      if (poll_total != kPollOpsPerPublisher ||
          poll_counts[p].value_canceled.load(std::memory_order_relaxed) !=
              kPollOpsPerPublisher ||
          poll_counts[p].value_ok.load(std::memory_order_relaxed) != 0 ||
          poll_counts[p].stopped.load(std::memory_order_relaxed) != 0 ||
          poll_counts[p].unexpected_error.load(std::memory_order_relaxed) !=
              0) {
        ::_exit(5);
      }
    }
  }
}

TEST(AbortingPublishRaceTest, publish_during_abort_window_completes_once) {
  EXPECT_TRUE(run_isolated(aborting_publish_body));
}

}  // namespace
