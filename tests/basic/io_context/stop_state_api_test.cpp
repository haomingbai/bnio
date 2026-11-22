// Lightweight API-level checks for the stop state:
//   - is_stopped() is false initially and true right after request_stop();
//   - request_stop() is idempotent;
//   - stop() keeps the legacy synchronous behavior with no worker: it
//     delivers queued work inline before returning.

#include <bnio/bnio.h>
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace {

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

TEST(StopStateApiTest, is_stopped_false_initially_and_true_after_request) {
  bnio::io_context ctx;
  EXPECT_FALSE(ctx.is_stopped());
  ctx.request_stop();
  EXPECT_TRUE(ctx.is_stopped());
}

TEST(StopStateApiTest, request_stop_is_idempotent) {
  bnio::io_context ctx;
  ctx.request_stop();
  ctx.request_stop();
  ctx.request_stop();
  EXPECT_TRUE(ctx.is_stopped());
}

TEST(StopStateApiTest, stop_without_worker_delivers_inline) {
  bnio::io_context ctx;
  constexpr int kOps = 4;
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

  // No worker ran, so nothing has been delivered yet.
  EXPECT_EQ(c.completed.load(std::memory_order_relaxed), 0);

  EXPECT_EQ(ctx.stop(), 0);

  // stop() with no worker is the synchronous fallback: every queued
  // operation was delivered before it returned.
  EXPECT_EQ(c.completed.load(std::memory_order_relaxed), kOps);
  EXPECT_EQ(c.canceled.load(std::memory_order_relaxed), kOps);
  EXPECT_EQ(c.stopped.load(std::memory_order_relaxed), 0);
}

// Phase 2 is already published: repeated stop(), request_stop(), and join()
// must all take the terminal fast path and return without delivering twice.
TEST(StopStateApiTest, stop_and_join_after_terminal_state_are_idempotent) {
  bnio::io_context ctx;
  EXPECT_EQ(ctx.stop(), 0);
  EXPECT_TRUE(ctx.is_stopped());

  EXPECT_EQ(ctx.stop(), 0);
  ctx.request_stop();

  std::atomic<bool> joined{false};
  auto join_sender = ctx.join();
  auto join_op = bexec::connect(join_sender, join_recv{&joined});
  bexec::start(join_op);
  EXPECT_TRUE(joined.load(std::memory_order_acquire));
  EXPECT_TRUE(ctx.is_stopped());

  // A third stop after join() still observes stopping and does not drain.
  EXPECT_EQ(ctx.stop(), 0);
}

// run() entered after Phase 2 completed is rejected at entry and reports
// operation_canceled; it must not block or re-run the terminal transition.
TEST(StopStateApiTest, run_after_completed_stop_is_rejected) {
  bnio::io_context ctx;
  EXPECT_EQ(ctx.stop(), 0);
  ASSERT_TRUE(ctx.is_stopped());

  const std::error_code result = ctx.run();
  EXPECT_EQ(result, std::make_error_code(std::errc::operation_canceled));
  EXPECT_TRUE(ctx.is_stopped());

  // The rejected run() released its worker slot; join() still returns.
  std::atomic<bool> joined{false};
  auto join_sender = ctx.join();
  auto join_op = bexec::connect(join_sender, join_recv{&joined});
  bexec::start(join_op);
  EXPECT_TRUE(joined.load(std::memory_order_acquire));
}

// Destroying after Phase 1 with no worker and no pending work is legal: the
// destructor publishes the terminal state and clears the completion thunk.
TEST(StopStateApiTest, destructor_after_request_stop_is_clean) {
  {
    bnio::io_context ctx;
    ctx.request_stop();
    EXPECT_TRUE(ctx.is_stopped());
  }
  // A fresh context is unaffected by the previous terminal publication.
  bnio::io_context next;
  EXPECT_FALSE(next.is_stopped());
}

}  // namespace
