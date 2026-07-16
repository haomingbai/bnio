#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "../../support/async_io/io_uring_context_test_support.h"

namespace {

using namespace bupp_async_io_io_uring_test;

struct concurrent_post_state {
  std::atomic<unsigned> completed{0};
  std::atomic<unsigned> stopped{0};
  std::atomic_bool all_in_context{true};
};

struct concurrent_post_receiver {
  std::shared_ptr<concurrent_post_state> state =
      std::make_shared<concurrent_post_state>();
  io_uring_context* context = nullptr;
  unsigned target = 0;

  void set_value() noexcept {
    if (context == nullptr || !context->is_in_context()) {
      state->all_in_context.store(false, std::memory_order_release);
    }
    const unsigned completed =
        state->completed.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (completed == target && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->stopped.fetch_add(1, std::memory_order_acq_rel);
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

TEST(IoUringRunLoopTest, posted_tasks_drain_in_post_order) {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  constexpr unsigned k_count = 8;
  auto state = std::make_shared<batch_state>();
  std::array<std::unique_ptr<io_uring_post_operation<post_batch_receiver>>,
             k_count>
      operations;

  for (unsigned index = 0; index < k_count; ++index) {
    post_batch_receiver recv;
    recv.context = &context;
    recv.target = k_count;
    recv.index = index;
    recv.state = state;
    operations[index] =
        std::make_unique<io_uring_post_operation<post_batch_receiver>>(
            context, std::move(recv));
    bexec::start(*operations[index]);
  }

  context.run();

  EXPECT_EQ(state->completed, k_count);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);
  EXPECT_TRUE(state->all_in_context);
  EXPECT_TRUE(state->in_order);
}

TEST(IoUringRunLoopTest, posted_tasks_accept_concurrent_external_posts) {
  io_uring_task_queue_state global_tasks;
  io_uring_context context;
  io_uring_context_options options;
  if (!queue_init_shared_or_skip(context, global_tasks, options)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  constexpr unsigned k_threads = 8;
  constexpr unsigned k_posts_per_thread = 128;
  constexpr unsigned k_count = k_threads * k_posts_per_thread;

  auto state = std::make_shared<concurrent_post_state>();
  std::vector<
      std::unique_ptr<io_uring_post_operation<concurrent_post_receiver>>>
      operations;
  operations.reserve(k_count);

  for (unsigned index = 0; index < k_count; ++index) {
    concurrent_post_receiver recv;
    recv.context = &context;
    recv.target = k_count;
    recv.state = state;
    operations.emplace_back(
        std::make_unique<io_uring_post_operation<concurrent_post_receiver>>(
            context, std::move(recv)));
  }

  std::thread runner([&context] { context.run(); });

  std::barrier ready(static_cast<std::ptrdiff_t>(k_threads + 1));
  std::vector<std::thread> producers;
  producers.reserve(k_threads);
  for (unsigned thread = 0; thread < k_threads; ++thread) {
    producers.emplace_back([&operations, &ready, thread] {
      ready.arrive_and_wait();
      const unsigned first = thread * k_posts_per_thread;
      const unsigned last = first + k_posts_per_thread;
      for (unsigned index = first; index < last; ++index) {
        bexec::start(*operations[index]);
      }
    });
  }

  ready.arrive_and_wait();

  for (std::thread& producer : producers) {
    producer.join();
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (state->completed.load(std::memory_order_acquire) != k_count &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (state->completed.load(std::memory_order_acquire) != k_count) {
    (void)context.stop();
  }
  runner.join();

  EXPECT_EQ(state->completed.load(std::memory_order_acquire), k_count);
  EXPECT_EQ(state->stopped.load(std::memory_order_acquire), 0);
  EXPECT_TRUE(state->all_in_context.load(std::memory_order_acquire));
}

TEST(IoUringRunLoopTest, shared_closing_state_finishes_the_worker) {
  io_uring_task_queue_state global_state;
  io_uring_context context;
  if (!queue_init_shared_or_skip(context, global_state)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  global_state.closing.store(true, std::memory_order_release);
  context.run();

  EXPECT_EQ(global_state.awake_workers.load(std::memory_order_acquire), 0);
}

}  // namespace
