#include <bnio/bnio.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Receiver that records success of an error_code channel.
struct ec_recv {
  std::atomic<bool>* flag = nullptr;
  void set_value(std::error_code ec) noexcept {
    if (flag) flag->store(!static_cast<bool>(ec), std::memory_order_release);
  }
  void set_stopped() noexcept {
    if (flag) flag->store(false, std::memory_order_release);
  }
};

// Receiver that stops a context and records completion.
struct stop_recv {
  bnio::io_context* ctx = nullptr;
  std::atomic<bool>* flag = nullptr;
  void set_value(std::error_code) noexcept {
    if (flag) flag->store(true, std::memory_order_release);
    if (ctx) ctx->stop();
  }
  void set_stopped() noexcept {
    if (flag) flag->store(false, std::memory_order_release);
  }
};

// Heap-based holders so async operations outlive their initiators.
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

// Receiver for the outer worker that drives the nested run() scenario.
struct nested_run_recv {
  bnio::io_context* inner_ctx = nullptr;
  bnio::io_context* outer_ctx = nullptr;
  std::atomic<bool>* inner_stop_task_done = nullptr;
  std::atomic<bool>* inner_run_returned = nullptr;
  std::atomic<bool>* outer_resumed = nullptr;
  std::vector<std::unique_ptr<op_holder_base>>* ops = nullptr;

  void set_value(std::error_code) noexcept {
    // 1. Post a task to the inner context that stops it, so the nested
    //    run() below returns naturally.
    auto inner_sched = inner_ctx->get_post_scheduler();
    auto inner_sender = inner_sched.schedule();
    using InnerOp = decltype(bexec::connect(inner_sender, stop_recv{nullptr}));
    auto inner_op = std::make_unique<op_holder<InnerOp>>(
        inner_sender, stop_recv{inner_ctx, inner_stop_task_done});
    bexec::start(inner_op->op);
    ops->push_back(std::move(inner_op));

    // 2. Run the inner context's event loop on this (outer worker) thread.
    std::error_code ec = inner_ctx->run();
    inner_run_returned->store(!static_cast<bool>(ec),
                              std::memory_order_release);

    // 3. The outer loop must keep working after the nested run returns:
    //    post one more task to it.
    auto outer_sched = outer_ctx->get_post_scheduler();
    auto outer_sender = outer_sched.schedule();
    using OuterOp = decltype(bexec::connect(outer_sender, ec_recv{nullptr}));
    auto outer_op = std::make_unique<op_holder<OuterOp>>(
        outer_sender, ec_recv{outer_resumed});
    bexec::start(outer_op->op);
    ops->push_back(std::move(outer_op));
  }

  void set_stopped() noexcept {
    if (inner_run_returned)
      inner_run_returned->store(false, std::memory_order_release);
  }
};

bool wait_for(const std::atomic<bool>& flag) {
  for (int i = 0; i < 5000 && !flag.load(std::memory_order_acquire); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return flag.load(std::memory_order_acquire);
}

// Nested run(): an outer worker's handler runs a second context's event
// loop to completion, then the outer loop keeps processing normally.
TEST(LifecycleTest, nested_run) {
  auto outer = std::make_unique<bnio::io_context>();
  auto inner = std::make_unique<bnio::io_context>();
  if (!outer->is_open() || !inner->is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  std::thread worker([&outer]() { outer->run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::atomic<bool> inner_stop_task_done{false};
  std::atomic<bool> inner_run_returned{false};
  std::atomic<bool> outer_resumed{false};

  std::vector<std::unique_ptr<op_holder_base>> ops;
  ops.reserve(4);

  auto outer_sched = outer->get_post_scheduler();
  auto sender = outer_sched.schedule();
  using Op = decltype(bexec::connect(sender, nested_run_recv{}));
  auto h = std::make_unique<op_holder<Op>>(
      sender, nested_run_recv{inner.get(), outer.get(), &inner_stop_task_done,
                              &inner_run_returned, &outer_resumed, &ops});
  // Push into `ops` before start(): the handler appends to `ops` from the
  // worker thread, so the main thread must not touch the vector afterwards.
  op_holder<Op>* op_ptr = h.get();
  ops.push_back(std::move(h));
  bexec::start(op_ptr->op);

  ASSERT_TRUE(wait_for(inner_run_returned));
  ASSERT_TRUE(wait_for(outer_resumed));

  outer->stop();
  worker.join();

  EXPECT_TRUE(inner_stop_task_done.load(std::memory_order_acquire));
  EXPECT_TRUE(inner_run_returned.load(std::memory_order_acquire));
  EXPECT_TRUE(outer_resumed.load(std::memory_order_acquire));
}

}  // namespace
