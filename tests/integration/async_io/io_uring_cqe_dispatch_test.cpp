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

// Minimal receiver that sets an atomic flag when the context run loop
// processes a no-op CQE, signalling that the run loop is active.
struct run_loop_signal_recv {
  std::atomic<bool>* started = nullptr;

  void set_value(std::error_code) noexcept {
    if (started) started->store(true, std::memory_order_release);
  }

  void set_value(std::error_code, int, unsigned) noexcept {
    if (started) started->store(true, std::memory_order_release);
  }

  void set_stopped() noexcept {
    if (started) started->store(true, std::memory_order_release);
  }
};

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

  // Post a no-op that sets an atomic flag when the run loop first
  // processes a CQE, confirming the run loop is active.
  std::atomic<bool> run_loop_started{false};
  run_loop_signal_recv sig_recv;
  sig_recv.started = &run_loop_started;
  auto signal_op =
      std::make_unique<io_uring_nop_operation<run_loop_signal_recv>>(
          context, std::move(sig_recv));
  bexec::start(*signal_op);

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

  // Spin-wait for the run loop to be confirmed active (5-second timeout).
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!run_loop_started.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() > deadline) {
      break;
    }
    std::this_thread::yield();
  }

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

  EXPECT_EQ(state->completed.load(std::memory_order_acquire), k_count);
  EXPECT_EQ(state->errors.load(std::memory_order_acquire), 0);
  EXPECT_EQ(state->stopped.load(std::memory_order_acquire), 0);
}

}  // namespace
