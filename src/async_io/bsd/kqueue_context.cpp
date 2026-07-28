/**
 * @file kqueue_context.cpp
 * @brief kqueue_context lifecycle and queue initialization.
 */

#include <bnio/async_io/bsd/kqueue_context.h>

#include <cassert>
#include <cerrno>
#include <limits>
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
  if (queue_initialized_) {
    return -EALREADY;
  }

  apply_context_options(options);
  const std::size_t entries = static_cast<std::size_t>(options_.entries);
  if (entries >
      std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(2)) {
    return -ENOMEM;
  }
  const std::size_t active_capacity = entries * 2;
  auto active = std::unique_ptr<active_registration[]>(
      new (std::nothrow) active_registration[active_capacity]);
  auto events = std::unique_ptr<bnio::base::event[]>(
      new (std::nothrow) bnio::base::event[options_.event_batch_window]);
  if (!active || !events) {
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

  local_state_.clear();
  incoming_io_tasks_ = nullptr;
  active_registration_capacity_ = active_capacity;
  next_registration_sequence_ = 0;
  active_registrations_ = std::move(active);
  event_buffer_ = std::move(events);
  run_active_.store(false, std::memory_order_release);
  waiting_.store(false, std::memory_order_release);
  queue_initialized_ = true;
  state_.store(context_state::running, std::memory_order_release);
  return 0;
}

void kqueue_context::queue_exit() noexcept {
  // Abort any remaining inflight I/O before closing the queue.
  // Normal shutdown cleans these up in finish(), but forced/abnormal
  // shutdown (e.g. closing flag, destruction without run) may leave
  // operations in-flight.
  abort_inflight_io();

  state_.store(context_state::finished, std::memory_order_release);

  local_state_.clear();
  incoming_io_tasks_ = nullptr;
  active_registrations_.reset();
  active_registration_capacity_ = 0;
  next_registration_sequence_ = 0;

  event_buffer_.reset();
  queue_.close();
  queue_initialized_ = false;
}

bool kqueue_context::is_open() const noexcept { return queue_.is_open(); }

void kqueue_context::set_global_state(kqueue_task_queue_state* state) noexcept {
  assert(!run_active_.load(std::memory_order_acquire));
  global_state_ = state;
}

void kqueue_context::assert_running() const noexcept {
#ifndef NDEBUG
  assert(state_.load(std::memory_order_acquire) == context_state::running);
#endif
}

}  // namespace bnio::async_io::bsd_native
