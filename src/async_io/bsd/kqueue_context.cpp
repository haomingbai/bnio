/**
 * @file kqueue_context.cpp
 * @brief kqueue_context lifecycle and queue initialization.
 */

#include <bnio/async_io/bsd/kqueue_context.h>

#include <cassert>
#include <cerrno>
#include <new>

#include "kqueue_context_internal.h"

namespace bnio::async_io::bsd_native {

thread_local kqueue_context* kqueue_context::current_context_ = nullptr;

kqueue_context::kqueue_context() noexcept = default;

kqueue_context::kqueue_context(const kqueue_context_options& options) noexcept {
  (void)queue_init(options);
}

kqueue_context::~kqueue_context() noexcept { queue_exit(); }

void kqueue_context::apply_context_options(
    const kqueue_context_options& options) noexcept {
  options_ = options;
  if (options_.entries == 0) {
    options_.entries = 1;
  }
  if (options_.event_batch_window == 0) {
    options_.event_batch_window = 1;
  }
}

int kqueue_context::queue_init(const kqueue_context_options& options) noexcept {
  if (run_state_.queue_initialized) {
    return -EALREADY;
  }

  apply_context_options(options);
  // The wait queues live inside registration nodes embedded in inflight
  // operations, so there is no per-registration table to allocate here.
  auto events = std::unique_ptr<bnio::base::event[]>(
      new (std::nothrow) bnio::base::event[options_.event_batch_window]);
  if (!events) {
    return -ENOMEM;
  }

  const int open_result = queue_.open();
  if (open_result < 0) {
    return open_result;
  }

  // The shared wake fd (an EVFILT_READ registration) is set up in run()
  // after set_global_state() provides the fd. Per-worker EVFILT_USER is
  // no longer created here — wake is now driven by the shared channel
  // owned by io_context.

  // Open the per-worker wake channel used for directed wakeups.
  (void)local_state_.wake_channel_.open();

  (void)local_state_.pop_cpu_all();
  local_io_head_ = nullptr;
  scheduling_state_.next_registration_sequence = 0;
  event_buffer_ = std::move(events);
  run_state_.run_active.store(false, std::memory_order_release);
  run_state_.waiting.store(false, std::memory_order_release);
  run_state_.queue_initialized = true;
  run_state_.state.store(context_state::running, std::memory_order_release);
  return 0;
}

void kqueue_context::queue_exit() noexcept {
  // Abort any remaining inflight I/O before closing the queue.
  // Normal shutdown cleans these up in finish(), but forced/abnormal
  // shutdown (e.g. closing flag, destruction without run) may leave
  // operations in-flight.
  abort_inflight_io();

  run_state_.state.store(context_state::finished, std::memory_order_release);

  (void)local_state_.pop_cpu_all();
  local_io_head_ = nullptr;
  scheduling_state_.next_registration_sequence = 0;

  local_state_.wake_channel_.close();
  event_buffer_.reset();
  queue_.close();
  run_state_.queue_initialized = false;
}

bool kqueue_context::is_open() const noexcept { return queue_.is_open(); }

void kqueue_context::set_global_state(kqueue_task_queue_state* state) noexcept {
  assert(!run_state_.run_active.load(std::memory_order_acquire));
  global_state_ = state;
}

void kqueue_context::assert_running() const noexcept {
#ifndef NDEBUG
  const context_state s = run_state_.state.load(std::memory_order_acquire);
  // The winning thread may legitimately observe finishing if stop() was
  // requested before run() on this native context (see io_context::run()).
  assert(s == context_state::running || s == context_state::finishing);
#endif
}

}  // namespace bnio::async_io::bsd_native
