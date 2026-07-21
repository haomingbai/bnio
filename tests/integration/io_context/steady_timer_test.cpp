#include <gtest/gtest.h>

#include <optional>
#include <thread>
#include <vector>

#include "../../support/io_context/io_context_runtime_test_support.h"

namespace {

struct ordered_timer_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bnio::io_context* context = nullptr;
  std::atomic<unsigned>* next_order = nullptr;
  std::atomic<unsigned>* observed_order = nullptr;
  std::atomic<unsigned>* completions = nullptr;

  void set_value() noexcept {
    state->signal = signal_kind::value;
    observed_order->store(
        next_order->fetch_add(1, std::memory_order_acq_rel) + 1,
        std::memory_order_release);
    complete();
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    complete();
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    complete();
  }

 private:
  void complete() noexcept {
    if (completions->fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
      (void)context->stop();
    }
  }
};

TEST(SteadyTimerTest, steady_timer_completes) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  bnio::steady_timer timer(context);
  EXPECT_EQ(timer.expires_after(std::chrono::milliseconds(1)), 0);

  void_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = timer.async_wait();
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
}

TEST(SteadyTimerTest, inactive_timer_wait_posts_completion_directly) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  // The default expiry is now, so registration puts this timer on the
  // inactive list. Starting a wait must post completion directly instead of
  // routing it through the time heap or timer driver.
  bnio::steady_timer timer(context);

  void_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto operation = bexec::connect(timer.async_wait(), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
}

TEST(SteadyTimerTest, steady_timer_cancel_stops_wait) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  bnio::steady_timer timer(context);
  EXPECT_EQ(timer.expires_after(std::chrono::seconds(30)), 0);

  void_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = timer.async_wait();
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  EXPECT_EQ(timer.cancel(), 1);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::stopped);
}

TEST(SteadyTimerTest, steady_timer_expires_after_stops_old_wait) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  bnio::steady_timer timer(context);
  EXPECT_EQ(timer.expires_after(std::chrono::seconds(30)), 0);

  void_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = timer.async_wait();
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  EXPECT_EQ(timer.expires_after(std::chrono::milliseconds(1)), 1);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::stopped);
}

TEST(SteadyTimerTest, steady_timer_rearms_after_cancel) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  bnio::steady_timer timer(context);
  EXPECT_EQ(timer.expires_after(std::chrono::seconds(30)), 0);

  unsigned completions = 0;
  void_receiver canceled_receiver;
  canceled_receiver.context = &context;
  canceled_receiver.completions = &completions;
  canceled_receiver.target = 2;
  auto canceled_state = canceled_receiver.state;
  auto canceled_operation =
      bexec::connect(timer.async_wait(), std::move(canceled_receiver));
  bexec::start(canceled_operation);

  EXPECT_EQ(timer.cancel(), 1);
  EXPECT_EQ(timer.expires_after(std::chrono::milliseconds(1)), 0);

  void_receiver rearmed_receiver;
  rearmed_receiver.context = &context;
  rearmed_receiver.completions = &completions;
  rearmed_receiver.target = 2;
  auto rearmed_state = rearmed_receiver.state;
  auto rearmed_operation =
      bexec::connect(timer.async_wait(), std::move(rearmed_receiver));
  bexec::start(rearmed_operation);

  context.run();

  EXPECT_EQ(completions, 2);
  EXPECT_EQ(canceled_state->signal, signal_kind::stopped);
  EXPECT_EQ(rearmed_state->signal, signal_kind::value);
}

TEST(SteadyTimerTest, steady_timer_multiple_waits_complete) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  bnio::steady_timer timer(context);
  EXPECT_EQ(timer.expires_after(std::chrono::milliseconds(1)), 0);

  unsigned completions = 0;
  void_receiver first;
  first.context = &context;
  first.completions = &completions;
  first.target = 2;
  auto first_state = first.state;

  void_receiver second;
  second.context = &context;
  second.completions = &completions;
  second.target = 2;
  auto second_state = second.state;

  auto first_sender = timer.async_wait();
  auto second_sender = timer.async_wait();
  auto first_operation =
      bexec::connect(std::move(first_sender), std::move(first));
  auto second_operation =
      bexec::connect(std::move(second_sender), std::move(second));
  bexec::start(first_operation);
  bexec::start(second_operation);
  context.run();

  EXPECT_EQ(completions, 2);
  EXPECT_EQ(first_state->signal, signal_kind::value);
  EXPECT_EQ(second_state->signal, signal_kind::value);
}

TEST(SteadyTimerTest, steady_timer_cancel_counts_all_queued_waits) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  bnio::steady_timer timer(context);
  EXPECT_EQ(timer.expires_after(std::chrono::seconds(30)), 0);

  unsigned completions = 0;
  void_receiver first;
  first.context = &context;
  first.completions = &completions;
  first.target = 2;
  auto first_state = first.state;

  void_receiver second;
  second.context = &context;
  second.completions = &completions;
  second.target = 2;
  auto second_state = second.state;

  auto first_operation = bexec::connect(timer.async_wait(), std::move(first));
  auto second_operation = bexec::connect(timer.async_wait(), std::move(second));
  bexec::start(first_operation);
  bexec::start(second_operation);

  EXPECT_EQ(timer.cancel(), 2);
  context.run();

  EXPECT_EQ(completions, 2);
  EXPECT_EQ(first_state->signal, signal_kind::stopped);
  EXPECT_EQ(second_state->signal, signal_kind::stopped);
}

TEST(SteadyTimerTest, timer_destruction_after_stop_does_not_post) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  std::optional<bnio::steady_timer> timer;
  timer.emplace(context);
  EXPECT_EQ(timer->expires_after(std::chrono::seconds(30)), 0);

  void_receiver receiver;
  auto operation = bexec::connect(timer->async_wait(), std::move(receiver));
  bexec::start(operation);

  EXPECT_GE(context.stop(), 0);
  timer.reset();
}

TEST(SteadyTimerTest, steady_timer_move_stops_old_wait) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  bnio::steady_timer timer(context);
  EXPECT_EQ(timer.expires_after(std::chrono::seconds(30)), 0);

  unsigned completions = 0;
  void_receiver receiver;
  receiver.context = &context;
  receiver.completions = &completions;
  receiver.target = 2;
  auto state = receiver.state;

  auto sender = timer.async_wait();
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  bnio::steady_timer moved_timer(std::move(timer));
  EXPECT_EQ(moved_timer.expires_after(std::chrono::milliseconds(1)), 0);

  void_receiver moved_receiver;
  moved_receiver.context = &context;
  moved_receiver.completions = &completions;
  moved_receiver.target = 2;
  auto moved_state = moved_receiver.state;
  auto moved_sender = moved_timer.async_wait();
  auto moved_operation =
      bexec::connect(std::move(moved_sender), std::move(moved_receiver));
  bexec::start(moved_operation);
  context.run();

  EXPECT_EQ(completions, 2);
  EXPECT_EQ(state->signal, signal_kind::stopped);
  EXPECT_EQ(moved_state->signal, signal_kind::value);
}

TEST(SteadyTimerTest, steady_timer_pre_stopped_token_stops_wait) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  bexec::inplace_stop_source source;
  EXPECT_TRUE(source.request_stop());
  bnio::steady_timer timer(context);
  EXPECT_EQ(timer.expires_after(std::chrono::seconds(30)), 0);

  stopped_void_receiver receiver;
  receiver.context = &context;
  receiver.env = stop_env{source.get_token()};
  auto state = receiver.state;

  auto sender = timer.async_wait();
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::stopped);
}

TEST(SteadyTimerTest,
     timer_update_stays_on_primary_context_with_multiple_workers) {
  constexpr unsigned worker_count = 4;
  bnio::io_context_options options;
  options.concurrency_hint = worker_count;
  bnio::io_context context(options);
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  std::atomic<unsigned> completions{0};
  std::atomic<unsigned> next_order{0};
  std::atomic<unsigned> first_order{0};
  std::atomic<unsigned> second_order{0};
  bnio::steady_timer first_timer(context);
  // Use a generous expiry so CI scheduling jitter cannot cause the first
  // timer to fire before the shorter second timer is even drained into the
  // timer heap by the primary worker.
  EXPECT_EQ(first_timer.expires_after(std::chrono::milliseconds(500)), 0);

  ordered_timer_receiver first_receiver;
  first_receiver.context = &context;
  first_receiver.completions = &completions;
  first_receiver.next_order = &next_order;
  first_receiver.observed_order = &first_order;
  auto first_state = first_receiver.state;
  auto first_operation =
      bexec::connect(first_timer.async_wait(), std::move(first_receiver));
  bexec::start(first_operation);

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (unsigned index = 0; index < worker_count; ++index) {
    workers.emplace_back([&context] { context.run(); });
  }

  // Give the primary worker enough time to drain the first timer submission
  // and settle before scheduling the second, shorter timer. On macOS CI
  // runners sleep_for can overshoot significantly, so a wide gap is used.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  bnio::steady_timer second_timer(context);
  EXPECT_EQ(second_timer.expires_after(std::chrono::milliseconds(20)), 0);

  ordered_timer_receiver second_receiver;
  second_receiver.context = &context;
  second_receiver.completions = &completions;
  second_receiver.next_order = &next_order;
  second_receiver.observed_order = &second_order;
  auto second_state = second_receiver.state;
  auto second_operation =
      bexec::connect(second_timer.async_wait(), std::move(second_receiver));
  bexec::start(second_operation);

  for (std::thread& worker : workers) {
    worker.join();
  }

  EXPECT_EQ(completions.load(std::memory_order_acquire), 2);
  EXPECT_EQ(first_state->signal, signal_kind::value);
  EXPECT_EQ(second_state->signal, signal_kind::value);
  EXPECT_EQ(second_order.load(std::memory_order_acquire), 1);
  EXPECT_EQ(first_order.load(std::memory_order_acquire), 2);
}

}  // namespace
