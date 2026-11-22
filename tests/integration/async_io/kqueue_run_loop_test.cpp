#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <bexec/operation_state.hpp>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "../../support/async_io/kqueue_context_test_support.h"

namespace {

using namespace bnio_async_io_kqueue_test;

struct concurrent_state {
  std::atomic<unsigned> completed{0};
  std::atomic<unsigned> stopped{0};
  std::atomic_bool all_in_context{true};
};

struct concurrent_receiver {
  std::shared_ptr<concurrent_state> state;
  kqueue_context* context = nullptr;
  unsigned target = 0;

  void set_value(std::error_code ec) noexcept {
    EXPECT_FALSE(ec);
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

struct batch_receiver {
  std::shared_ptr<concurrent_state> state;
  kqueue_context* context = nullptr;
  unsigned target = 0;

  void set_value(std::error_code ec, int result, unsigned /*flags*/) noexcept {
    EXPECT_FALSE(ec);
    EXPECT_EQ(result, 0);
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

TEST(KqueueRunLoopTest, passive_io_queue_registers_all_published_operations) {
  kqueue_context context;
  kqueue_context_options options;
  options.entries = 8;
  options.event_inline_completion_threshold = 0;
  options.local_queue_threshold = 2;
  EXPECT_EQ(context.queue_init(options), 0);

  constexpr unsigned operation_count = 8;
  auto state = std::make_shared<concurrent_state>();
  std::vector<std::unique_ptr<kqueue_nop_operation<batch_receiver>>> operations;
  operations.reserve(operation_count);
  for (unsigned index = 0; index < operation_count; ++index) {
    batch_receiver completion{state, &context, operation_count};
    operations.push_back(std::make_unique<kqueue_nop_operation<batch_receiver>>(
        context, std::move(completion)));
  }

  for (auto& operation : operations) {
    bexec::start(*operation);
  }
  context.run();

  EXPECT_EQ(state->completed.load(std::memory_order_acquire), operation_count);
  EXPECT_EQ(state->stopped.load(std::memory_order_acquire), 0);
  EXPECT_TRUE(state->all_in_context.load(std::memory_order_acquire));
}

TEST(KqueueRunLoopTest, concurrent_external_io_publication_is_drained) {
  kqueue_task_queue_state global_tasks;
  EXPECT_GE(global_tasks.wake_channel_.open(), 0);
  kqueue_context context;
  kqueue_context_options options;
  options.wait_spin_count = 1;
  context.set_global_state(&global_tasks);
  EXPECT_EQ(context.queue_init(options), 0);

  constexpr unsigned thread_count = 4;
  constexpr unsigned operations_per_thread = 128;
  constexpr unsigned operation_count = thread_count * operations_per_thread;
  auto state = std::make_shared<concurrent_state>();

  std::vector<std::unique_ptr<kqueue_nop_operation<batch_receiver>>> operations;
  operations.reserve(operation_count);
  for (unsigned index = 0; index < operation_count; ++index) {
    batch_receiver completion{state, &context, operation_count};
    operations.push_back(std::make_unique<kqueue_nop_operation<batch_receiver>>(
        context, std::move(completion)));
  }

  std::thread runner([&context] { context.run(); });
  std::barrier ready(static_cast<std::ptrdiff_t>(thread_count + 1));
  std::vector<std::thread> producers;
  producers.reserve(thread_count);
  for (unsigned thread = 0; thread < thread_count; ++thread) {
    producers.emplace_back([&operations, &ready, thread] {
      ready.arrive_and_wait();
      const unsigned first = thread * operations_per_thread;
      const unsigned last = first + operations_per_thread;
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
  while (state->completed.load(std::memory_order_acquire) != operation_count &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (state->completed.load(std::memory_order_acquire) != operation_count) {
    (void)context.stop();
  }
  runner.join();

  EXPECT_EQ(state->completed.load(std::memory_order_acquire), operation_count);
  EXPECT_EQ(state->stopped.load(std::memory_order_acquire), 0);
  EXPECT_TRUE(state->all_in_context.load(std::memory_order_acquire));
}

TEST(KqueueRunLoopTest, concurrent_external_posts_are_drained) {
  kqueue_task_queue_state global_tasks;
  EXPECT_GE(global_tasks.wake_channel_.open(), 0);
  kqueue_context context;
  kqueue_context_options options;
  options.wait_spin_count = 1;
  context.set_global_state(&global_tasks);
  EXPECT_EQ(context.queue_init(options), 0);

  constexpr unsigned thread_count = 4;
  constexpr unsigned posts_per_thread = 128;
  constexpr unsigned operation_count = thread_count * posts_per_thread;
  auto state = std::make_shared<concurrent_state>();

  std::vector<std::unique_ptr<kqueue_post_operation<concurrent_receiver>>>
      operations;
  operations.reserve(operation_count);
  for (unsigned index = 0; index < operation_count; ++index) {
    concurrent_receiver completion{state, &context, operation_count};
    operations.push_back(
        std::make_unique<kqueue_post_operation<concurrent_receiver>>(
            context, std::move(completion)));
  }

  std::thread runner([&context] { context.run(); });
  std::barrier ready(static_cast<std::ptrdiff_t>(thread_count + 1));
  std::vector<std::thread> producers;
  producers.reserve(thread_count);
  for (unsigned thread = 0; thread < thread_count; ++thread) {
    producers.emplace_back([&operations, &ready, thread] {
      ready.arrive_and_wait();
      const unsigned first = thread * posts_per_thread;
      const unsigned last = first + posts_per_thread;
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
  while (state->completed.load(std::memory_order_acquire) != operation_count &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (state->completed.load(std::memory_order_acquire) != operation_count) {
    (void)context.stop();
  }
  runner.join();

  EXPECT_EQ(state->completed.load(std::memory_order_acquire), operation_count);
  EXPECT_EQ(state->stopped.load(std::memory_order_acquire), 0);
  EXPECT_TRUE(state->all_in_context.load(std::memory_order_acquire));
}

TEST(KqueueRunLoopTest, shared_closing_state_finishes_the_worker) {
  kqueue_task_queue_state global_state;
  kqueue_context context;
  context.set_global_state(&global_state);
  EXPECT_EQ(context.queue_init(), 0);

  global_state.life_state.store(
      static_cast<int>(bnio::async_io::bsd_native::stop_transition::stopping),
      std::memory_order_release);
  context.run();

  EXPECT_EQ(global_state.awake_workers.load(std::memory_order_acquire), 0);
}

// Two-phase stop: aborting (phase 2 in progress) is not yet terminal. A
// worker that wakes while the shared state is aborting must go back to
// waiting; only stopping may end the run loop.
TEST(KqueueRunLoopTest, shared_aborting_state_does_not_finish_the_worker) {
  kqueue_task_queue_state global_state;
  ASSERT_GE(global_state.wake_channel_.open(), 0);
  kqueue_context context;
  context.set_global_state(&global_state);
  EXPECT_EQ(context.queue_init(), 0);

  std::atomic<bool> finished{false};
  std::thread worker([&context, &finished] {
    context.run();
    finished.store(true, std::memory_order_release);
  });

  const auto park_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!context.is_waiting() &&
         std::chrono::steady_clock::now() < park_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!context.is_waiting()) {
    (void)context.stop();
    worker.join();
    FAIL() << "worker never parked in the run loop wait";
    return;
  }

  // Wake the worker while the shared state is still aborting: phase 2 has
  // aborted timer waits but has not published stopping yet.
  global_state.life_state.store(
      static_cast<int>(bnio::async_io::bsd_native::stop_transition::aborting),
      std::memory_order_release);
  (void)global_state.wake_channel_.wake();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_FALSE(finished.load(std::memory_order_acquire))
      << "worker finished while the shared state was aborting";

  // Publishing stopping is what permits finish().
  global_state.life_state.store(
      static_cast<int>(bnio::async_io::bsd_native::stop_transition::stopping),
      std::memory_order_release);
  (void)global_state.wake_channel_.wake();

  const auto finish_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!finished.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < finish_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(finished.load(std::memory_order_acquire))
      << "worker did not finish after the stopping state was published";
  if (!finished.load(std::memory_order_acquire)) {
    (void)context.stop();
  }
  worker.join();

  EXPECT_EQ(global_state.awake_workers.load(std::memory_order_acquire), 0);
}

}  // namespace
