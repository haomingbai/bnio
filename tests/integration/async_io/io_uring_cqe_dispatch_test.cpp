#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "../../support/async_io/io_uring_context_test_support.h"

namespace {

using namespace bnio_async_io_io_uring_test;

TEST(IoUringCqeDispatchTest, cqe_batch_window_drains_multiple_rounds) {
  io_uring_context context;
  io_uring_context_options options;
  options.entries = 16;
  options.cqe_batch_window = 2;
  options.wait_spin_count = 1;
  options.cqe_inline_completion_threshold = 0;
  if (!queue_init_or_skip(context, options)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  constexpr unsigned k_count = 5;
  auto state = std::make_shared<batch_state>();
  std::array<std::unique_ptr<io_uring_nop_operation<batch_receiver>>, k_count>
      operations;

  for (auto& operation : operations) {
    batch_receiver recv;
    recv.context = &context;
    recv.target = k_count;
    recv.state = state;
    operation = std::make_unique<io_uring_nop_operation<batch_receiver>>(
        context, std::move(recv));
    bexec::start(*operation);
  }

  context.run();

  EXPECT_EQ(state->completed, k_count);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);
  EXPECT_TRUE(state->all_in_context);
}

TEST(IoUringCqeDispatchTest, concurrent_start_uses_the_single_run_thread) {
  io_uring_task_queue_state global_tasks;
  io_uring_context context;
  io_uring_context_options options;
  options.entries = 1024;
  options.cqe_batch_window = 1;
  options.wait_spin_count = 1024;
  options.cqe_inline_completion_threshold = 0;
  options.local_queue_threshold = 8;
  if (!queue_init_shared_or_skip(context, global_tasks, options)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  constexpr unsigned k_count = 512;
  constexpr unsigned k_threads = 4;
  auto state = std::make_shared<concurrent_batch_state>();

  // Completion counter whose receiver never calls stop(): the context is
  // stopped by the main thread only after every losing worker has
  // returned from run(), so no worker's run_active CAS can ever land
  // outside the run loop lifetime.
  struct drain_only_recv {
    std::shared_ptr<concurrent_batch_state> state;

    void set_value(std::error_code ec, int result, unsigned) noexcept {
      if (ec) {
        state->errors.fetch_add(1, std::memory_order_acq_rel);
      } else {
        EXPECT_TRUE(result == 0);
        state->completed.fetch_add(1, std::memory_order_acq_rel);
      }
    }

    void set_stopped() noexcept {
      state->stopped.fetch_add(1, std::memory_order_acq_rel);
    }
  };

  // Start every operation before releasing the workers so the run loop
  // enters with all k_count operations already inflight.
  //
  // Once the context reaches finished, calling run() again is not
  // supported: a worker whose run_active CAS is delayed past the run
  // loop's lifetime would win the CAS and hit the finished assertion.
  // The receivers therefore never stop the context; the main thread
  // waits until all operations completed and k_threads - 1 workers (the
  // losers; the winner is still inside run()) have returned from run()
  // before calling stop(). Until then run_active stays true, so every
  // worker CAS lands inside the run loop lifetime.
  std::vector<std::unique_ptr<io_uring_nop_operation<drain_only_recv>>>
      operations;
  operations.reserve(k_count);
  for (unsigned index = 0; index < k_count; ++index) {
    drain_only_recv recv;
    recv.state = state;
    operations.push_back(
        std::make_unique<io_uring_nop_operation<drain_only_recv>>(
            context, std::move(recv)));
    bexec::start(*operations.back());
  }

  std::atomic<unsigned> workers_returned{0};
  std::barrier ready(static_cast<std::ptrdiff_t>(k_threads + 1));
  std::vector<std::thread> workers;
  workers.reserve(k_threads);
  for (unsigned index = 0; index < k_threads; ++index) {
    workers.emplace_back([&context, &ready, &workers_returned] {
      ready.arrive_and_wait();
      context.run();
      workers_returned.fetch_add(1, std::memory_order_acq_rel);
    });
  }

  ready.arrive_and_wait();

  // Wait until every operation completed and the losing workers (all but
  // the run-loop winner, which is still inside run()) have returned, so
  // their CAS attempts are done, then stop the context (5-second timeout
  // guards against an unexpected fatal run-loop exit).
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((state->completed.load(std::memory_order_acquire) < k_count ||
          workers_returned.load(std::memory_order_acquire) < k_threads - 1) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  (void)context.stop();

  for (auto& worker : workers) {
    worker.join();
  }

  EXPECT_EQ(state->completed.load(std::memory_order_acquire), k_count);
  EXPECT_EQ(state->errors.load(std::memory_order_acquire), 0);
  EXPECT_EQ(state->stopped.load(std::memory_order_acquire), 0);
}

}  // namespace
