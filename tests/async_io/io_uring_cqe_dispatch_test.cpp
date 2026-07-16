#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "io_uring_context_test_support.h"

namespace {

using namespace bupp_async_io_io_uring_test;

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

  EXPECT_TRUE(state->completed == k_count);
  EXPECT_TRUE(state->errors == 0);
  EXPECT_TRUE(state->stopped == 0);
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

  std::barrier ready(static_cast<std::ptrdiff_t>(k_threads + 1));
  std::vector<std::thread> workers;
  workers.reserve(k_threads);
  for (unsigned index = 0; index < k_threads; ++index) {
    workers.emplace_back([&context, &ready] {
      ready.arrive_and_wait();
      context.run();
    });
  }

  ready.arrive_and_wait();
  std::this_thread::sleep_for(std::chrono::milliseconds(25));

  std::vector<
      std::unique_ptr<io_uring_nop_operation<concurrent_batch_receiver>>>
      operations;
  operations.reserve(k_count);
  for (unsigned index = 0; index < k_count; ++index) {
    concurrent_batch_receiver recv;
    recv.context = &context;
    recv.target = k_count;
    recv.state = state;
    operations.push_back(
        std::make_unique<io_uring_nop_operation<concurrent_batch_receiver>>(
            context, std::move(recv)));
    bexec::start(*operations.back());
  }

  for (auto& worker : workers) {
    worker.join();
  }

  EXPECT_TRUE(state->completed.load(std::memory_order_acquire) == k_count);
  EXPECT_TRUE(state->errors.load(std::memory_order_acquire) == 0);
  EXPECT_TRUE(state->stopped.load(std::memory_order_acquire) == 0);
}

}  // namespace
