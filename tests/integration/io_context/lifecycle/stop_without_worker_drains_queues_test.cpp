#include <bnio/bnio.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <poll.h>
#include <utility>
#include <vector>

namespace {

// Terminal-state bookkeeping with one counter per channel and error value:
// merged counters would hide a wrong-channel delivery (docs/usage/index.md
// requires set_value(operation_canceled) for stop-aborted work and forbids
// set_stopped when no stop token races).
struct terminal_counts {
  std::atomic<int> value_ok{0};
  std::atomic<int> value_canceled{0};
  std::atomic<int> stopped{0};
};

struct schedule_recv {
  terminal_counts* counts = nullptr;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      counts->value_canceled.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    counts->value_ok.fetch_add(1, std::memory_order_relaxed);
  }

  void set_stopped() noexcept {
    counts->stopped.fetch_add(1, std::memory_order_relaxed);
  }
};

struct poll_recv {
  terminal_counts* counts = nullptr;

  void set_value(std::error_code ec, unsigned) noexcept {
    if (ec) {
      counts->value_canceled.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    counts->value_ok.fetch_add(1, std::memory_order_relaxed);
  }

  void set_stopped() noexcept {
    counts->stopped.fetch_add(1, std::memory_order_relaxed);
  }
};

struct timer_recv {
  terminal_counts* counts = nullptr;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      counts->value_canceled.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    counts->value_ok.fetch_add(1, std::memory_order_relaxed);
  }

  void set_stopped() noexcept {
    counts->stopped.fetch_add(1, std::memory_order_relaxed);
  }
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

// Contract (docs/design/lifecycle.md, "stop() drains the shared queues"):
// stop() must deliver a terminal state to every operation that was
// published before the stopping state was elected, even when no worker
// ever entered run(). Queued schedule operations and shared-queued I/O
// complete with set_value(operation_canceled); aborted timer waits staged
// on timers_.ready complete with set_value(operation_canceled) as well.
TEST(StopWithoutWorkerTest, stop_without_worker_delivers_queued_operations) {
  auto ctx = std::make_unique<bnio::io_context>();
  if (!ctx->is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  constexpr int kScheduleCount = 100;
  constexpr int kPollCount = 8;

  terminal_counts schedule_counts;
  terminal_counts poll_counts;
  terminal_counts timer_counts;

  std::vector<std::unique_ptr<op_holder_base>> ops;

  // Queued CPU work: posted schedules sitting on the shared CPU queue.
  auto sched = ctx->get_post_scheduler();
  for (int i = 0; i < kScheduleCount; ++i) {
    auto sender = sched.schedule();
    using Op = decltype(bexec::connect(sender, schedule_recv{nullptr}));
    auto holder = std::make_unique<op_holder<Op>>(
        sender, schedule_recv{&schedule_counts});
    bexec::start(holder->op);
    ops.push_back(std::move(holder));
  }

  // Queued I/O work: polls on an invalid descriptor published to the
  // shared I/O queue. With no worker, nothing ever registers them with
  // the native backend; descriptor_view(-1) is never touched before the
  // drain delivers them.
  auto scheduler = ctx->get_post_scheduler();
  for (int i = 0; i < kPollCount; ++i) {
    auto sender = bnio::async_poll(
        scheduler, bnio::async_io::descriptor_view(-1),
        static_cast<unsigned>(POLLIN));
    using Op = decltype(bexec::connect(sender, poll_recv{nullptr}));
    auto holder =
        std::make_unique<op_holder<Op>>(sender, poll_recv{&poll_counts});
    bexec::start(holder->op);
    ops.push_back(std::move(holder));
  }

  // One far-future timer wait: stop()'s abort stages it on timers_.ready,
  // and with no worker nobody else will ever consume it.
  const auto far_deadline =
      std::chrono::steady_clock::now() + std::chrono::hours(24);
  bnio::steady_timer timer(*ctx);
  ASSERT_EQ(timer.expires_at(far_deadline), 0U);
  auto timer_sender = timer.async_wait();
  using TimerOp = decltype(bexec::connect(timer_sender, timer_recv{nullptr}));
  auto timer_op = std::make_unique<op_holder<TimerOp>>(
      timer_sender, timer_recv{&timer_counts});
  bexec::start(timer_op->op);

  // No run() was ever started; stop() must still deliver everything.
  EXPECT_GE(ctx->stop(), 0);

  EXPECT_EQ(schedule_counts.value_canceled.load(std::memory_order_relaxed),
            kScheduleCount);
  EXPECT_EQ(schedule_counts.value_ok.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(schedule_counts.stopped.load(std::memory_order_relaxed), 0);

  EXPECT_EQ(poll_counts.value_canceled.load(std::memory_order_relaxed),
            kPollCount);
  EXPECT_EQ(poll_counts.value_ok.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(poll_counts.stopped.load(std::memory_order_relaxed), 0);

  EXPECT_EQ(timer_counts.value_canceled.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(timer_counts.value_ok.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(timer_counts.stopped.load(std::memory_order_relaxed), 0);
}

}  // namespace
