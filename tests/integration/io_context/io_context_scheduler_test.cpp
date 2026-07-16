#include <gtest/gtest.h>

#include <thread>

#include "../../support/io_context/io_context_runtime_test_support.h"

namespace {

TEST(IoContextSchedulerTest, post_scheduler_schedule_posts_fifo) {
  bupp::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  auto scheduler = context.get_post_scheduler();
  auto state = std::make_shared<schedule_state>();
  state->order.reserve(3);

  schedule_receiver first;
  first.state = state;
  first.context = &context;
  first.value = 1;
  first.target = 3;

  schedule_receiver second;
  second.state = state;
  second.context = &context;
  second.value = 2;
  second.target = 3;

  schedule_receiver third;
  third.state = state;
  third.context = &context;
  third.value = 3;
  third.target = 3;

  auto first_operation =
      bexec::connect(bexec::schedule(scheduler), std::move(first));
  auto second_operation =
      bexec::connect(bexec::schedule(scheduler), std::move(second));
  auto third_operation =
      bexec::connect(bexec::schedule(scheduler), std::move(third));

  bexec::start(first_operation);
  bexec::start(second_operation);
  bexec::start(third_operation);

  EXPECT_TRUE(state->order.empty());
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->order.size(), 3);
  EXPECT_EQ(state->order[0], 1);
  EXPECT_EQ(state->order[1], 2);
  EXPECT_EQ(state->order[2], 3);
}

TEST(IoContextSchedulerTest,
     dispatch_scheduler_schedule_posts_outside_context) {
  bupp::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  auto scheduler = context.get_dispatch_scheduler();
  auto state = std::make_shared<schedule_state>();
  state->order.reserve(1);

  schedule_receiver receiver;
  receiver.state = state;
  receiver.context = &context;
  receiver.value = 7;

  auto operation =
      bexec::connect(bexec::schedule(scheduler), std::move(receiver));
  bexec::start(operation);

  EXPECT_EQ(state->signal, signal_kind::none);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->order.size(), 1);
  EXPECT_EQ(state->order[0], 7);
}

TEST(IoContextSchedulerTest,
     dispatch_scheduler_schedule_runs_inline_in_context) {
  bupp::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  auto state = std::make_shared<schedule_state>();
  state->order.reserve(1);

  auto operation =
      bexec::connect(bexec::schedule(context.get_post_scheduler()),
                     dispatch_inline_outer_receiver{state, &context});
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_TRUE(state->completed_during_start);
  EXPECT_EQ(state->order.size(), 1);
  EXPECT_EQ(state->order[0], 42);
}

TEST(IoContextSchedulerTest, scheduler_schedule_pre_stopped_token_stops) {
  bupp::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  bexec::inplace_stop_source source;
  EXPECT_TRUE(source.request_stop());

  auto state = std::make_shared<schedule_state>();
  stopped_schedule_receiver receiver;
  receiver.state = state;
  receiver.context = &context;
  receiver.env = stop_env{source.get_token()};

  auto operation = bexec::connect(bexec::schedule(context.get_post_scheduler()),
                                  std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::stopped);
}

TEST(IoContextSchedulerTest, default_context_runs_on_a_different_thread) {
  bupp::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  auto state = std::make_shared<schedule_state>();
  schedule_receiver receiver;
  receiver.state = state;
  receiver.context = &context;
  receiver.value = 99;

  auto operation = bexec::connect(bexec::schedule(context.get_post_scheduler()),
                                  std::move(receiver));
  bexec::start(operation);

  std::thread runner([&context] { context.run(); });
  runner.join();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->order.size(), 1);
  EXPECT_EQ(state->order[0], 99);
}

}  // namespace
