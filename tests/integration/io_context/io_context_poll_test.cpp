#include <gtest/gtest.h>

#include "../../support/io_context/io_context_runtime_test_support.h"

#include <system_error>

namespace {

// Contract (docs/usage/index.md): the `unsigned` ready mask is meaningful only
// on success. On failure or cancellation the mask is 0 and the error travels in
// the leading `ec`, so a caller's `mask & POLLIN` test can never trip on a
// negative native result wrapped into `unsigned`.
TEST(IoContextPollTest, poll_observes_pipe_readiness) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int descriptors[2] = {-1, -1};
#if defined(BNIO_SYSTEM_LINUX)
  EXPECT_EQ(::pipe2(descriptors, O_CLOEXEC), 0);
#else
  EXPECT_EQ(::pipe(descriptors), 0);
  EXPECT_EQ(::fcntl(descriptors[0], F_SETFD, FD_CLOEXEC), 0);
  EXPECT_EQ(::fcntl(descriptors[1], F_SETFD, FD_CLOEXEC), 0);
#endif

  poll_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = bnio::async_poll(
      scheduler, bnio::async_io::descriptor_view(descriptors[0]),
      static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  constexpr char byte = 'q';
  EXPECT_EQ(::write(descriptors[1], &byte, sizeof(byte)),
            static_cast<ssize_t>(sizeof(byte)));
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_TRUE((static_cast<unsigned>(state->size) &
               static_cast<unsigned>(POLLIN)) != 0);

  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

// An invalid descriptor never reaches a kqueue wait: registration fails with
// EV_RECEIPT -> EBADF, which routes through complete_submit_error into the
// value_with_ec branch. ec carries EBADF; the mask must be 0, not
// static_cast<unsigned>(-EBADF).
TEST(IoContextPollTest, poll_error_reports_zero_mask) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  poll_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender =
      bnio::async_poll(scheduler, bnio::async_io::descriptor_view(-1),
                       static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::error);
  EXPECT_NE(state->error, std::error_code{});
  EXPECT_EQ(state->poll_events, 0u);
}

// Stops the context from inside the run loop.
struct poll_stopper_receiver {
  bnio::io_context* context = nullptr;

  void set_value(std::error_code) noexcept { (void)context->stop(); }
  void set_stopped() noexcept { (void)context->stop(); }
};

// Nothing is ever written to the pipe, so the poll can only complete through
// the shutdown abort. The stopper task requests stop() from inside the loop, so
// the poll is aborted either while registered (abort_inflight_io ->
// complete_submit_stopped) or while still queued (drained as stopped). Either
// way the receiver's stop token is never cancelled, so execute() arbitrates to
// set_value(operation_canceled, 0).
TEST(IoContextPollTest, poll_cancel_reports_zero_mask) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int descriptors[2] = {-1, -1};
#if defined(BNIO_SYSTEM_LINUX)
  EXPECT_EQ(::pipe2(descriptors, O_CLOEXEC), 0);
#else
  EXPECT_EQ(::pipe(descriptors), 0);
  EXPECT_EQ(::fcntl(descriptors[0], F_SETFD, FD_CLOEXEC), 0);
  EXPECT_EQ(::fcntl(descriptors[1], F_SETFD, FD_CLOEXEC), 0);
#endif

  // context stays null: the completion must not self-stop, the stopper does it.
  poll_receiver receiver;
  auto state = receiver.state;

  auto sender = bnio::async_poll(
      scheduler, bnio::async_io::descriptor_view(descriptors[0]),
      static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  auto stopper_sender = scheduler.schedule();
  auto stopper_operation =
      bexec::connect(stopper_sender, poll_stopper_receiver{&context});
  bexec::start(stopper_operation);

  context.run();

  EXPECT_EQ(state->signal, signal_kind::error);
  EXPECT_EQ(state->error, std::make_error_code(std::errc::operation_canceled));
  EXPECT_EQ(state->poll_events, 0u);

  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

}  // namespace
