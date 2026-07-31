#include <bnio/io_context.h>
#include <gtest/gtest.h>

#include <atomic>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

#include "../../support/io_context/io_context_runtime_test_support.h"

namespace {

struct timer_stress_state {
  std::atomic<unsigned> completions{0};
  std::atomic<unsigned> errors{0};
  std::atomic<unsigned> stopped{0};
};

struct timer_stress_receiver {
  timer_stress_state* state;
  bnio::io_context* context;
  unsigned target_completions = 0;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      state->errors.fetch_add(1, std::memory_order_relaxed);
    }
    complete();
  }
  void set_stopped() noexcept {
    state->stopped.fetch_add(1, std::memory_order_relaxed);
    complete();
  }

 private:
  void complete() noexcept {
    unsigned completed =
        state->completions.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (completed >= target_completions && context != nullptr) {
      (void)context->stop();
    }
  }
};

struct count_only_receiver {
  timer_stress_state* state;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      state->errors.fetch_add(1, std::memory_order_relaxed);
    }
    state->completions.fetch_add(1, std::memory_order_acq_rel);
  }
  void set_stopped() noexcept {
    state->stopped.fetch_add(1, std::memory_order_relaxed);
    state->completions.fetch_add(1, std::memory_order_acq_rel);
  }
};

struct safety_stop_receiver {
  bnio::io_context* context;

  void set_value(std::error_code) noexcept { (void)context->stop(); }
  void set_stopped() noexcept { (void)context->stop(); }
};

}  // namespace

TEST(TimerStressTest, many_short_lived_timers) {
  bnio::io_context_options options;
  constexpr unsigned worker_count = 2;
  options.concurrency_hint = worker_count;
  bnio::io_context context(options);
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  constexpr int num_timers = 500;
  constexpr unsigned target = static_cast<unsigned>(num_timers);

  timer_stress_state state;
  std::vector<bnio::steady_timer> timers;
  timers.reserve(static_cast<std::size_t>(num_timers));

  using wait_sender_type =
      decltype(std::declval<bnio::steady_timer&>().async_wait());
  using wait_op_type = decltype(bexec::connect(
      std::declval<wait_sender_type>(), std::declval<timer_stress_receiver>()));

  std::vector<std::unique_ptr<wait_op_type>> ops;
  ops.resize(static_cast<std::size_t>(num_timers));

  for (int i = 0; i < num_timers; ++i) {
    std::size_t idx = static_cast<std::size_t>(i);
    timers.emplace_back(context);
    (void)timers.back().expires_after(std::chrono::milliseconds(1));
    ops[idx].reset(new wait_op_type(
        bexec::connect(timers.back().async_wait(),
                       timer_stress_receiver{&state, &context, target})));
    bexec::start(*ops[idx]);
  }

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (unsigned i = 0; i < worker_count; ++i) {
    workers.emplace_back([&context] { context.run(); });
  }
  for (auto& w : workers) {
    w.join();
  }

  EXPECT_EQ(state.completions.load(std::memory_order_acquire), target);
  EXPECT_EQ(state.errors.load(std::memory_order_acquire), 0);
  EXPECT_EQ(state.stopped.load(std::memory_order_acquire), 0);
}

TEST(TimerStressTest, concurrent_timer_rekey) {
  bnio::io_context_options options;
  constexpr unsigned worker_count = 4;
  options.concurrency_hint = worker_count;
  bnio::io_context context(options);
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  (void)context.get_post_scheduler();

  std::srand(42);

  constexpr int num_timers = 100;
  constexpr int rekeys_per_timer = 10;
  constexpr unsigned target =
      static_cast<unsigned>(num_timers * rekeys_per_timer);

  timer_stress_state state;
  std::vector<bnio::steady_timer> timers;
  timers.reserve(static_cast<std::size_t>(num_timers));
  for (int i = 0; i < num_timers; ++i) {
    timers.emplace_back(context);
  }

  // rekey_receiver chains timer wait operations through pre-allocated slots.
  // Since the operation type depends on the receiver, we use a void* array
  // for operation storage to break the circular type dependency.
  struct rekey_receiver {
    timer_stress_state* state;
    bnio::io_context* context;
    std::vector<bnio::steady_timer>* timers;
    int rekeys_per_timer;
    unsigned target;
    int timer_index;
    int round;
    std::vector<void*>* ops_storage;

    void set_value(std::error_code ec) noexcept {
      unsigned total =
          state->completions.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (ec) {
        state->errors.fetch_add(1, std::memory_order_relaxed);
      } else {
        int next_round = round + 1;
        if (next_round < rekeys_per_timer) {
          auto& timer = (*timers)[static_cast<std::size_t>(timer_index)];
          (void)timer.expires_after(
              std::chrono::milliseconds(std::rand() % 20 + 1));

          using next_op_type = decltype(bexec::connect(
              std::declval<decltype(timer.async_wait())>(),
              std::declval<rekey_receiver>()));

          int op_idx = timer_index * rekeys_per_timer + next_round;
          auto* op = new next_op_type(bexec::connect(
              timer.async_wait(),
              rekey_receiver{state, context, timers, rekeys_per_timer, target,
                             timer_index, next_round, ops_storage}));
          (*ops_storage)[static_cast<std::size_t>(op_idx)] = op;
          bexec::start(*op);
        }
      }
      if (total >= target && context != nullptr) {
        (void)context->stop();
      }
    }

    void set_stopped() noexcept {
      state->stopped.fetch_add(1, std::memory_order_relaxed);
      unsigned total =
          state->completions.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (total >= target && context != nullptr) {
        (void)context->stop();
      }
    }
  };

  std::vector<void*> ops_storage(static_cast<std::size_t>(target), nullptr);

  for (int i = 0; i < num_timers; ++i) {
    (void)timers[static_cast<std::size_t>(i)].expires_after(
        std::chrono::milliseconds(std::rand() % 20 + 1));
    auto& timer = timers[static_cast<std::size_t>(i)];

    using init_op_type =
        decltype(bexec::connect(std::declval<decltype(timer.async_wait())>(),
                                std::declval<rekey_receiver>()));

    int op_idx = i * rekeys_per_timer;
    auto* op = new init_op_type(bexec::connect(
        timer.async_wait(),
        rekey_receiver{&state, &context, &timers, rekeys_per_timer, target, i,
                       0, &ops_storage}));
    ops_storage[static_cast<std::size_t>(op_idx)] = op;
    bexec::start(*op);
  }

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (unsigned i = 0; i < worker_count; ++i) {
    workers.emplace_back([&context] { context.run(); });
  }
  for (auto& w : workers) {
    w.join();
  }

  EXPECT_EQ(state.completions.load(std::memory_order_acquire), target);
  EXPECT_EQ(state.errors.load(std::memory_order_acquire), 0);
  EXPECT_EQ(state.stopped.load(std::memory_order_acquire), 0);

  // Clean up
  for (void* ptr : ops_storage) {
    delete static_cast<char*>(ptr);  // type-erased cleanup
  }
}

TEST(TimerStressTest, many_concurrent_timers) {
  bnio::io_context_options options;
  constexpr unsigned worker_count = 4;
  options.concurrency_hint = worker_count;
  bnio::io_context context(options);
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  std::srand(123);

  constexpr int num_timers = 200;

  timer_stress_state state;
  std::vector<bnio::steady_timer> timers;
  timers.reserve(static_cast<std::size_t>(num_timers));

  using wait_sender_type =
      decltype(std::declval<bnio::steady_timer&>().async_wait());
  using wait_op_type = decltype(bexec::connect(
      std::declval<wait_sender_type>(), std::declval<count_only_receiver>()));

  std::vector<std::unique_ptr<wait_op_type>> ops;
  ops.resize(static_cast<std::size_t>(num_timers));

  for (int i = 0; i < num_timers; ++i) {
    std::size_t idx = static_cast<std::size_t>(i);
    timers.emplace_back(context);
    (void)timers.back().expires_after(
        std::chrono::milliseconds(std::rand() % 51));
    ops[idx].reset(new wait_op_type(bexec::connect(
        timers.back().async_wait(), count_only_receiver{&state})));
    bexec::start(*ops[idx]);
  }

  // Safety timer: stops context after 200ms
  bnio::steady_timer safety_timer(context);
  (void)safety_timer.expires_after(std::chrono::milliseconds(200));

  using safety_sender_type = decltype(safety_timer.async_wait());
  using safety_op_type =
      decltype(bexec::connect(std::declval<safety_sender_type>(),
                              std::declval<safety_stop_receiver>()));

  auto safety_op =
      std::unique_ptr<safety_op_type>(new safety_op_type(bexec::connect(
          safety_timer.async_wait(), safety_stop_receiver{&context})));
  bexec::start(*safety_op);

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (unsigned i = 0; i < worker_count; ++i) {
    workers.emplace_back([&context] { context.run(); });
  }
  for (auto& w : workers) {
    w.join();
  }

  EXPECT_GE(state.completions.load(std::memory_order_acquire), 190);
  EXPECT_EQ(state.errors.load(std::memory_order_acquire), 0);
}
