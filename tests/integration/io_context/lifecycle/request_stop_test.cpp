// Regression tests for the phase-1 API request_stop():
//   - it flips is_stopped() immediately, is idempotent, and takes no lock;
//   - it alone never delivers queued work (no abort, no publish, no drain);
//   - work published between request_stop() and phase 2 is still accepted and
//     delivered exactly once by join()'s drain;
//   - run() entered after request_stop() is rejected and drains before its
//     operation_canceled return;
//   - request_stop() from a receiver returns immediately and the worker
//     itself drives phase 2 and exits.
//
// The expected failure before the implementation lands is the request_stop()
// stub in src/posix/io_context.cpp: is_stopped() never flips and a run loop
// that relies on the worker-driven phase 2 keeps waiting.

#include <bnio/bnio.h>
#include <gtest/gtest.h>
#include <poll.h>

#include <atomic>
#include <chrono>
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

// Terminal-channel counters; set_stopped is always a contract violation
// because no receiver here carries a stop token.
struct terminal_counts {
  std::atomic<int> value_ok{0};
  std::atomic<int> value_canceled{0};
  std::atomic<int> stopped{0};

  void record(std::error_code ec) noexcept {
    if (ec == std::make_error_code(std::errc::operation_canceled)) {
      value_canceled.fetch_add(1, std::memory_order_relaxed);
    } else if (!ec) {
      value_ok.fetch_add(1, std::memory_order_relaxed);
    } else {
      // Unexpected error code: count as a contract violation.
      stopped.fetch_add(1, std::memory_order_relaxed);
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

// Calls request_stop() from inside a worker callback, then records that the
// call returned. run() must end once the worker drives phase 2.
struct request_stop_recv {
  bnio::io_context* ctx;
  std::atomic<bool>* entered;
  std::atomic<bool>* request_returned;

  void set_value(std::error_code) noexcept {
    entered->store(true, std::memory_order_release);
    ctx->request_stop();
    request_returned->store(true, std::memory_order_release);
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

struct queued_work {
  std::vector<std::unique_ptr<op_holder_base>> holders;
  std::unique_ptr<bnio::steady_timer> timer;
  terminal_counts schedule_counts;
  terminal_counts poll_counts;
  terminal_counts timer_counts;

  void publish(bnio::io_context& ctx, int schedules, int polls,
               bool with_timer) {
    auto scheduler = ctx.get_post_scheduler();
    for (int i = 0; i < schedules; ++i) {
      auto sender = scheduler.schedule();
      using Op = decltype(bexec::connect(sender, schedule_recv{nullptr}));
      holders.push_back(std::make_unique<op_holder<Op>>(
          sender, schedule_recv{&schedule_counts}));
      bexec::start(static_cast<op_holder<Op>*>(holders.back().get())->op);
    }
    for (int i = 0; i < polls; ++i) {
      auto sender =
          bnio::async_poll(scheduler, bnio::async_io::descriptor_view(-1),
                           static_cast<unsigned>(POLLIN));
      using Op = decltype(bexec::connect(sender, poll_recv{nullptr}));
      holders.push_back(
          std::make_unique<op_holder<Op>>(sender, poll_recv{&poll_counts}));
      bexec::start(static_cast<op_holder<Op>*>(holders.back().get())->op);
    }
    if (with_timer) {
      timer = std::make_unique<bnio::steady_timer>(ctx);
      const auto far_deadline = std::chrono::steady_clock::now() + 24h;
      if (timer->expires_at(far_deadline) != 0U) {
        ADD_FAILURE() << "failed to arm the long timer";
        return;
      }
      auto sender = timer->async_wait();
      using Op = decltype(bexec::connect(sender, timer_recv{nullptr}));
      holders.push_back(
          std::make_unique<op_holder<Op>>(sender, timer_recv{&timer_counts}));
      bexec::start(static_cast<op_holder<Op>*>(holders.back().get())->op);
    }
  }
};

TEST(RequestStopTest, is_stopped_false_initially_and_true_after_request) {
  bnio::io_context ctx;
  EXPECT_FALSE(ctx.is_stopped());
  ctx.request_stop();
  EXPECT_TRUE(ctx.is_stopped());
}

TEST(RequestStopTest, request_stop_is_nonblocking_and_idempotent) {
  bnio::io_context ctx;
  std::atomic<bool> go{false};
  constexpr int kThreads = 4;
  constexpr int kCalls = 1000;
  std::vector<std::thread> callers;
  callers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    callers.emplace_back([&ctx, &go] {
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int i = 0; i < kCalls; ++i) {
        ctx.request_stop();
      }
    });
  }

  const auto start = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  for (std::thread& caller : callers) {
    caller.join();
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, 2s) << "request_stop() blocked a caller";
  EXPECT_TRUE(ctx.is_stopped());
}

TEST(RequestStopTest, request_stop_alone_does_not_deliver_pending_work) {
  bnio::io_context ctx;
  queued_work work;
  work.publish(ctx, /*schedules=*/16, /*polls=*/8, /*with_timer=*/true);

  ctx.request_stop();

  // Phase 1 is not a drain: with no worker nobody may deliver the queued
  // work yet.
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(work.schedule_counts.value_canceled.load(), 0);
  EXPECT_EQ(work.schedule_counts.value_ok.load(), 0);
  EXPECT_EQ(work.poll_counts.value_canceled.load(), 0);
  EXPECT_EQ(work.poll_counts.value_ok.load(), 0);
  EXPECT_EQ(work.timer_counts.value_canceled.load(), 0);
  EXPECT_EQ(work.timer_counts.value_ok.load(), 0);

  // join() drives phase 2 and drains, delivering every operation exactly
  // once as operation_canceled.
  std::atomic<bool> joined{false};
  auto join_sender = ctx.join();
  auto join_op = bexec::connect(join_sender, join_recv{&joined});
  bexec::start(join_op);

  EXPECT_TRUE(joined.load(std::memory_order_acquire));
  EXPECT_EQ(work.schedule_counts.value_canceled.load(), 16);
  EXPECT_EQ(work.schedule_counts.value_ok.load(), 0);
  EXPECT_EQ(work.poll_counts.value_canceled.load(), 8);
  EXPECT_EQ(work.poll_counts.value_ok.load(), 0);
  EXPECT_EQ(work.timer_counts.value_canceled.load(), 1);
  EXPECT_EQ(work.timer_counts.value_ok.load(), 0);
  EXPECT_EQ(work.schedule_counts.stopped.load(), 0);
  EXPECT_EQ(work.poll_counts.stopped.load(), 0);
  EXPECT_EQ(work.timer_counts.stopped.load(), 0);
}

TEST(RequestStopTest, request_stop_before_run_returns_canceled_and_drains) {
  bnio::io_context ctx;
  queued_work work;
  work.publish(ctx, /*schedules=*/1, /*polls=*/0, /*with_timer=*/true);

  ctx.request_stop();
  ASSERT_TRUE(ctx.is_stopped());

  std::atomic<bool> done{false};
  std::error_code run_result{};
  std::thread runner([&ctx, &done, &run_result] {
    run_result = ctx.run();
    done.store(true, std::memory_order_release);
  });

  const bool returned =
      wait_until([&] { return done.load(std::memory_order_acquire); }, 3s);
  if (!returned) {
    // The stub did not reject entry: unblock the loop so the test can report
    // the wrong outcome instead of hanging.
    (void)ctx.stop();
    runner.join();
    FAIL() << "run() did not return after request_stop() before entry";
    return;
  }
  runner.join();

  EXPECT_EQ(run_result, std::make_error_code(std::errc::operation_canceled));
  EXPECT_EQ(work.schedule_counts.value_canceled.load(), 1);
  EXPECT_EQ(work.schedule_counts.value_ok.load(), 0);
  EXPECT_EQ(work.timer_counts.value_canceled.load(), 1);
  EXPECT_EQ(work.timer_counts.value_ok.load(), 0);
  EXPECT_EQ(work.schedule_counts.stopped.load(), 0);
  EXPECT_EQ(work.timer_counts.stopped.load(), 0);
}

TEST(RequestStopTest,
     submit_between_request_and_complete_is_accepted_and_drained) {
  bnio::io_context ctx;
  ctx.request_stop();
  ASSERT_TRUE(ctx.is_stopped());

  // After phase 1 but before phase 2 the lifecycle is still running, so the
  // publish primitives accept the work. join() then aborts it exactly once.
  queued_work work;
  work.publish(ctx, /*schedules=*/8, /*polls=*/4, /*with_timer=*/true);

  std::atomic<bool> joined{false};
  auto join_sender = ctx.join();
  auto join_op = bexec::connect(join_sender, join_recv{&joined});
  bexec::start(join_op);

  EXPECT_TRUE(joined.load(std::memory_order_acquire));
  EXPECT_EQ(work.schedule_counts.value_canceled.load(), 8);
  EXPECT_EQ(work.poll_counts.value_canceled.load(), 4);
  EXPECT_EQ(work.timer_counts.value_canceled.load(), 1);
  EXPECT_EQ(work.schedule_counts.value_ok.load(), 0);
  EXPECT_EQ(work.poll_counts.value_ok.load(), 0);
  EXPECT_EQ(work.timer_counts.value_ok.load(), 0);
  EXPECT_EQ(work.schedule_counts.stopped.load(), 0);
  EXPECT_EQ(work.poll_counts.stopped.load(), 0);
  EXPECT_EQ(work.timer_counts.stopped.load(), 0);
}

TEST(RequestStopTest, request_stop_from_receiver_returns_immediately) {
  bnio::io_context ctx;
  std::atomic<bool> entered{false};
  std::atomic<bool> request_returned{false};

  auto scheduler = ctx.get_post_scheduler();
  auto sender = scheduler.schedule();
  using Op = decltype(bexec::connect(
      sender, request_stop_recv{nullptr, nullptr, nullptr}));
  auto operation = std::make_unique<op_holder<Op>>(
      sender, request_stop_recv{&ctx, &entered, &request_returned});
  bexec::start(operation->op);

  std::atomic<bool> run_done{false};
  std::error_code run_result{};
  std::thread worker([&ctx, &run_done, &run_result] {
    run_result = ctx.run();
    run_done.store(true, std::memory_order_release);
  });

  if (!wait_until([&] {
        return entered.load(std::memory_order_acquire) &&
               request_returned.load(std::memory_order_acquire);
      })) {
    (void)ctx.stop();
    worker.join();
    FAIL() << "the receiver never completed its request_stop() call";
    return;
  }

  // The worker that observed the request must drive phase 2 itself; no
  // external stop()/join() is involved.
  const bool exited =
      wait_until([&] { return run_done.load(std::memory_order_acquire); }, 3s);
  if (!exited) {
    (void)ctx.stop();
  }
  worker.join();

  EXPECT_TRUE(exited) << "run() stayed in the loop after request_stop() from "
                         "a receiver";
  // A run() that entered normally and was later stopped reports success.
  EXPECT_FALSE(static_cast<bool>(run_result));
  EXPECT_EQ(run_result.value(), 0);
  EXPECT_TRUE(ctx.is_stopped());
}

}  // namespace
