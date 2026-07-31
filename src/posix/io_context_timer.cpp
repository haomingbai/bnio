/**
 * @file io_context_timer.cpp
 * @brief io_context timer registration, cancellation, completion queue
 * management, and timeout fetch.
 */

#include <bnio/io_context.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>

#include "bnio/async_io/time.h"
#include "bnio/detail/posix/io_context/steady_timer.h"
#include "bnio/detail/posix/io_context/timer_types.h"

namespace bnio {

namespace detail {

timer_operation_base::timer_operation_base(io_context& context) noexcept
    : timer_context_(&context) {}

}  // namespace detail

void io_context::register_timer(detail::timer_slot& timer) noexcept {
  bool wake_worker = false;
  {
    std::lock_guard context_lock(timers_.mutex);
    if (timer.context != nullptr) {
      return;
    }

    timer.context = this;
    timer.submitted = {};
    timer.previous = nullptr;
    timer.child = nullptr;
    timer.next = nullptr;
    const time_point previous_deadline = timers_.heap_deadline();
    // If the timer has already expired, place it directly in the inactive
    // (ready) list so it fires immediately. Otherwise insert it into the
    // pairing heap ordered by expiry.
    if (timer.expiry > clock::now()) {
      timers_.push_heap(timer);
      wake_worker = timers_.heap_deadline() < previous_deadline;
    } else {
      timers_.push_inactive(timer);
    }
  }

  if (wake_worker) {
    wake_one_if_all_workers_sleeping();
  }
}

void io_context::unregister_timer(detail::timer_slot& timer) noexcept {
  bool wake_worker = false;
  {
    std::lock_guard context_lock(timers_.mutex);
    if (timer.context != this) {
      return;
    }

    if (timer.active) {
      timers_.erase_heap(timer);
    } else {
      timers_.erase_inactive(timer);
    }
    timer.context = nullptr;
    const detail::timer_operation_queue canceled =
        take_timer_operations_locked(timer);
    wake_worker = canceled.head != nullptr;
    enqueue_timer_operations_locked(canceled.head,
                                    detail::timer_completion_kind::canceled);
  }

  if (wake_worker) {
    wake_one_if_all_workers_sleeping();
  }
}

std::size_t io_context::cancel_timer(detail::timer_slot& timer) noexcept {
  std::size_t count = 0;
  bool wake_worker = false;
  {
    std::lock_guard context_lock(timers_.mutex);
    if (timer.context != this) {
      return 0;
    }

    const detail::timer_operation_queue canceled =
        take_timer_operations_locked(timer);
    count = canceled.size;
    wake_worker = canceled.head != nullptr;
    enqueue_timer_operations_locked(canceled.head,
                                    detail::timer_completion_kind::canceled);
  }

  if (wake_worker) {
    wake_one_if_all_workers_sleeping();
  }
  return count;
}

std::size_t io_context::set_timer_expiry(detail::timer_slot& timer,
                                         time_point expiry) noexcept {
  std::size_t count = 0;
  bool wake_worker = false;
  {
    std::lock_guard context_lock(timers_.mutex);
    if (timer.context != this) {
      return 0;
    }

    const time_point previous_deadline = timers_.heap_deadline();
    const bool was_active = timer.active;
    if (was_active) {
      timers_.erase_heap(timer);
    } else {
      timers_.erase_inactive(timer);
    }

    timer.expiry = expiry;
    const detail::timer_operation_queue canceled =
        take_timer_operations_locked(timer);
    count = canceled.size;
    const bool has_canceled_operations = canceled.head != nullptr;
    enqueue_timer_operations_locked(canceled.head,
                                    detail::timer_completion_kind::canceled);

    if (timer.expiry > clock::now()) {
      timers_.push_heap(timer);
    } else {
      timers_.push_inactive(timer);
    }
    wake_worker =
        has_canceled_operations || timers_.heap_deadline() < previous_deadline;
  }

  if (wake_worker) {
    wake_one_if_all_workers_sleeping();
  }
  return count;
}

io_context::time_point io_context::timer_expiry(
    const detail::timer_slot& timer) const noexcept {
  std::lock_guard context_lock(timers_.mutex);
  return timer.expiry;
}

void io_context::start_timer_wait(detail::timer_operation_base& operation,
                                  detail::timer_slot& timer) noexcept {
  bool wake_worker = false;
  {
    std::lock_guard context_lock(timers_.mutex);
    if (timer.context != this) {
      operation.timer_next_ = nullptr;
      enqueue_timer_operations_locked(&operation,
                                      detail::timer_completion_kind::canceled);
      wake_worker = true;
    } else if (!timer.active) {
      operation.timer_next_ = nullptr;
      enqueue_timer_operations_locked(&operation,
                                      detail::timer_completion_kind::value);
      wake_worker = true;
    } else {
      operation.timer_next_ = timer.submitted.head;
      timer.submitted.head = &operation;
      ++timer.submitted.size;
      return;
    }
  }

  if (wake_worker) {
    wake_one_if_all_workers_sleeping();
  }
}

detail::timer_operation_queue io_context::take_timer_operations_locked(
    detail::timer_slot& timer) noexcept {
  // Drain all submitted wait operations from this timer slot into a detached
  // queue. Caller must hold timers_.mutex.
  const detail::timer_operation_queue operations = timer.submitted;
  timer.submitted = {};
  return operations;
}

void io_context::enqueue_timer_operations_locked(
    detail::timer_operation_base* operations,
    detail::timer_completion_kind completion) noexcept {
  // Prepend each operation to the ready list, setting the completion kind
  // so the run loop can dispatch them. Caller must hold timers_.mutex.
  while (operations != nullptr) {
    detail::timer_operation_base* const operation = operations;
    operations = operation->timer_next_;
    operation->timer_completion_ = completion;
    operation->timer_next_ = timers_.ready;
    timers_.ready = operation;
  }
}

void io_context::queue_timer_completion(
    detail::timer_operation_base& operation,
    detail::timer_completion_kind completion) noexcept {
  {
    std::lock_guard context_lock(timers_.mutex);
    operation.timer_next_ = nullptr;
    enqueue_timer_operations_locked(&operation, completion);
  }
  wake_one_if_all_workers_sleeping();
}

bool io_context::try_fetch_timeout_operations(
    time_point& deadline, detail::native_operation_base*& operations) noexcept {
  operations = nullptr;
  // Use a lock-free CAS gate to serialize concurrent workers trying to
  // drain the timer heap. Workers that lose the CAS skip the expensive
  // heap walk entirely.
  bool expected = false;
  if (!timers_.timeout_fetching.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }

  // The CAS gate won, now attempt the structural lock. If the mutex is
  // already held, release the gate and return so the caller can retry.
  std::unique_lock context_lock(timers_.mutex, std::try_to_lock);
  if (!context_lock.owns_lock()) {
    timers_.timeout_fetching.store(false, std::memory_order_release);
    return false;
  }

  const time_point now = clock::now();
  while (timers_.heap_deadline() <= now) {
    detail::timer_slot* const timer = timers_.pop_heap();
    if (timer == nullptr) {
      break;
    }

    const detail::timer_operation_queue ready =
        take_timer_operations_locked(*timer);
    timers_.push_inactive(*timer);
    enqueue_timer_operations_locked(ready.head,
                                    detail::timer_completion_kind::value);
  }

  deadline = timers_.heap_deadline();
  detail::timer_operation_base* ready = timers_.ready;
  timers_.ready = nullptr;
  context_lock.unlock();
  timers_.timeout_fetching.store(false, std::memory_order_release);

  while (ready != nullptr) {
    detail::timer_operation_base* const operation = ready;
    ready = operation->timer_next_;
    operation->timer_next_ = nullptr;
    operation->next = operations;
    operations = operation;
  }
  return true;
}

bool io_context::try_fetch_timeout_operations_thunk(
    void* state, time_point& deadline,
    detail::native_operation_base*& operations) noexcept {
  auto* timers = static_cast<detail::timer_state_data*>(state);
  if (timers == nullptr || timers->owner == nullptr) {
    operations = nullptr;
    return false;
  }
  return timers->owner->try_fetch_timeout_operations(deadline, operations);
}

steady_timer::steady_timer(io_context& context) noexcept {
  timer_.expiry = clock::now();
  context.register_timer(timer_);
}

steady_timer::steady_timer(io_context& context, time_point expiry) noexcept {
  timer_.expiry = expiry;
  context.register_timer(timer_);
}

steady_timer::~steady_timer() noexcept {
  if (timer_.context != nullptr) {
    timer_.context->unregister_timer(timer_);
  }
}

steady_timer::steady_timer(steady_timer&& other) noexcept {
  io_context* context = other.timer_.context;
  time_point expiry = clock::now();
  if (context != nullptr) {
    expiry = context->timer_expiry(other.timer_);
    context->unregister_timer(other.timer_);
    timer_.expiry = expiry;
    context->register_timer(timer_);
  }
}

steady_timer& steady_timer::operator=(steady_timer&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  if (timer_.context != nullptr) {
    timer_.context->unregister_timer(timer_);
  }

  io_context* context = other.timer_.context;
  if (context != nullptr) {
    const time_point expiry = context->timer_expiry(other.timer_);
    context->unregister_timer(other.timer_);
    timer_.expiry = expiry;
    context->register_timer(timer_);
  }
  return *this;
}

steady_timer::time_point steady_timer::expiry() const noexcept {
  return timer_.context->timer_expiry(timer_);
}

std::size_t steady_timer::expires_at(time_point expiry) noexcept {
  return timer_.context->set_timer_expiry(timer_, expiry);
}

std::size_t steady_timer::cancel() noexcept {
  return timer_.context->cancel_timer(timer_);
}

void io_context::abort_pending_timer_waits() noexcept {
  std::lock_guard context_lock(timers_.mutex);
  // Drain all active timers from the heap.
  while (auto* slot = timers_.pop_heap()) {
    const detail::timer_operation_queue canceled =
        take_timer_operations_locked(*slot);
    slot->context = nullptr;
    enqueue_timer_operations_locked(canceled.head,
                                    detail::timer_completion_kind::stopped);
  }
  // Drain inactive (already-expired) timers.
  while (timers_.inactive != nullptr) {
    detail::timer_slot* slot = timers_.inactive;
    timers_.erase_inactive(*slot);
    slot->context = nullptr;
    const detail::timer_operation_queue canceled =
        take_timer_operations_locked(*slot);
    enqueue_timer_operations_locked(canceled.head,
                                    detail::timer_completion_kind::stopped);
  }
}

}  // namespace bnio
