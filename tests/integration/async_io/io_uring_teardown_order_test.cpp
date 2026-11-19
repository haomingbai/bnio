#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>

#include "../../support/async_io/io_uring_context_test_support.h"

namespace {

using namespace bnio_async_io_io_uring_test;

// One-shot receiver for the trigger read: the pipe it reads is already
// readable, so it completes normally and forces teardown from inside the
// run loop while the second read is still inflight in the kernel.
struct teardown_trigger_receiver {
  io_uring_context* context = nullptr;
  bool* delivered = nullptr;
  std::error_code* error = nullptr;

  void set_value(std::error_code ec, int /*result*/,
                 unsigned /*flags*/) noexcept {
    *delivered = true;
    if (error != nullptr) {
      *error = ec;
    }
    context->queue_exit();
  }

  void set_stopped() noexcept {
    *delivered = true;
    context->queue_exit();
  }
};

enum class delivery_kind {
  none,
  value,
  error,
  stopped,
};

// Observes the cancellation of the still-inflight read.  The receiver runs
// synchronously from queue_exit(), so is_open() reports whether the ring had
// already been closed at delivery time.
struct abort_observer_receiver {
  io_uring_context* context = nullptr;
  bool* delivered = nullptr;
  bool* ring_open_at_delivery = nullptr;
  delivery_kind* kind = nullptr;
  std::error_code* error = nullptr;

  void set_value(std::error_code ec, int /*result*/,
                 unsigned /*flags*/) noexcept {
    *delivered = true;
    *ring_open_at_delivery = context->is_open();
    if (ec) {
      *kind = delivery_kind::error;
      *error = ec;
    } else {
      *kind = delivery_kind::value;
    }
  }

  void set_stopped() noexcept {
    *delivered = true;
    *ring_open_at_delivery = context->is_open();
    *kind = delivery_kind::stopped;
  }
};

// Heap-owned so a teardown that never returns can be leaked instead of
// destroyed underneath the live run thread (see the bounded wait below).
struct teardown_owner {
  // Declared before the context so it is destroyed after it: queue_exit()
  // dereferences the shared state during the context destructor.
  std::unique_ptr<io_uring_task_queue_state> global_tasks;
  std::unique_ptr<io_uring_context> context;
  std::unique_ptr<io_uring_read_operation<teardown_trigger_receiver>>
      trigger_op;
  std::unique_ptr<io_uring_read_operation<abort_observer_receiver>> pending_op;
  std::array<char, 16> trigger_buffer{};
  std::array<char, 16> pending_buffer{};
  int trigger_pipe[2] = {-1, -1};
  int pending_pipe[2] = {-1, -1};

  bool trigger_delivered = false;
  std::error_code trigger_error;
  bool pending_delivered = false;
  bool pending_ring_open_at_delivery = false;
  delivery_kind pending_kind = delivery_kind::none;
  std::error_code pending_error;

  std::atomic<bool> run_finished{false};
  std::thread runner;
};

// Regression for the io_uring unclean-teardown ordering: queue_exit() must
// close the ring before it delivers the operation_canceled completion.
// Before the fix the inflight read was aborted (and its receiver executed)
// while the ring was still open, so the kernel could still access the buffer
// the receiver was about to release.
TEST(IoUringTeardownOrderTest, queue_exit_closes_ring_before_cancel_delivery) {
  auto owner = std::make_unique<teardown_owner>();
  owner->global_tasks = std::make_unique<io_uring_task_queue_state>();
  owner->context = std::make_unique<io_uring_context>();
  if (!queue_init_shared_or_skip(*owner->context, *owner->global_tasks)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  ASSERT_EQ(::pipe2(owner->trigger_pipe, O_CLOEXEC), 0);
  ASSERT_EQ(::pipe2(owner->pending_pipe, O_CLOEXEC), 0);
  constexpr char byte = 'x';
  ASSERT_EQ(::write(owner->trigger_pipe[1], &byte, sizeof(byte)),
            static_cast<ssize_t>(sizeof(byte)));

  teardown_trigger_receiver trigger_recv;
  trigger_recv.context = owner->context.get();
  trigger_recv.delivered = &owner->trigger_delivered;
  trigger_recv.error = &owner->trigger_error;
  owner->trigger_op =
      std::make_unique<io_uring_read_operation<teardown_trigger_receiver>>(
          *owner->context, descriptor_view(owner->trigger_pipe[0]),
          buffer_view(owner->trigger_buffer.data(),
                      owner->trigger_buffer.size()),
          std::move(trigger_recv));

  abort_observer_receiver pending_recv;
  pending_recv.context = owner->context.get();
  pending_recv.delivered = &owner->pending_delivered;
  pending_recv.ring_open_at_delivery = &owner->pending_ring_open_at_delivery;
  pending_recv.kind = &owner->pending_kind;
  pending_recv.error = &owner->pending_error;
  owner->pending_op =
      std::make_unique<io_uring_read_operation<abort_observer_receiver>>(
          *owner->context, descriptor_view(owner->pending_pipe[0]),
          buffer_view(owner->pending_buffer.data(),
                      owner->pending_buffer.size()),
          std::move(pending_recv));

  // Both reads reach the same consume_io_tasks() batch, so the pending read
  // is kernel-inflight before the trigger read's completion runs.
  owner->trigger_op->start();
  owner->pending_op->start();

  // Capture the pointee, not the owning unique_ptr: the timeout path below
  // leaks the pointee while this thread may still be running.
  teardown_owner* const state = owner.get();
  owner->runner = std::thread([state] {
    state->context->run();
    state->run_finished.store(true, std::memory_order_release);
  });

  // Bounded wait: the trigger read is immediately ready and queue_exit()
  // marks the context finished, so a healthy run returns in milliseconds.
  // On timeout the owner is intentionally leaked: the parked run thread may
  // still reference it, and process exit reaps both.
  constexpr auto kRunBound = std::chrono::seconds(5);
  const auto deadline = std::chrono::steady_clock::now() + kRunBound;
  while (!owner->run_finished.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (!owner->run_finished.load(std::memory_order_acquire)) {
    ADD_FAILURE() << "run() did not return within "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         kRunBound)
                         .count()
                  << "ms: queue_exit() from the completion receiver did not "
                     "finish the run loop";
    teardown_owner* leaked = owner.release();
    leaked->runner.detach();
    return;
  }
  owner->runner.join();

  ASSERT_TRUE(owner->trigger_delivered)
      << "the immediately-readable read never completed";
  EXPECT_FALSE(owner->trigger_error)
      << "the trigger read unexpectedly failed: " << owner->trigger_error;

  ASSERT_TRUE(owner->pending_delivered)
      << "the inflight read was never aborted and delivered";
  EXPECT_EQ(owner->pending_kind, delivery_kind::error);
  EXPECT_EQ(owner->pending_error,
            std::make_error_code(std::errc::operation_canceled));
  // The ordering under test: the ring must already be closed when the
  // cancellation reaches the receiver.
  EXPECT_FALSE(owner->pending_ring_open_at_delivery)
      << "queue_exit() delivered operation_canceled while the ring was still "
         "open; the kernel could still write through the receiver's buffer";

  (void)::close(owner->trigger_pipe[0]);
  (void)::close(owner->trigger_pipe[1]);
  (void)::close(owner->pending_pipe[0]);
  (void)::close(owner->pending_pipe[1]);
}

}  // namespace
