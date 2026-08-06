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

bool io_context::register_timer_locked(detail::timer_slot& timer,
                                       time_point now) noexcept {
  // Caller must hold timers_.mutex.
  if (timer.context != nullptr) {
    return false;
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
  if (timer.expiry > now) {
    timers_.push_heap(timer);
    return timers_.heap_deadline() < previous_deadline;
  }
  timers_.push_inactive(timer);
  return false;
}

void io_context::register_timer(detail::timer_slot& timer) noexcept {
  bool wake_worker = false;
  // Read the clock before taking timers_.mutex so the (relatively expensive)
  // clock call overlaps lock acquisition instead of lengthening the critical
  // section. steady_clock is monotonic, so a stale now read can only make the
  // heap-vs-inactive decision more conservative (a timer that expired while we
  // waited for the lock lands in the heap and fires on the next fetch cycle)
  // and can never cause an early fire.
  const time_point now = clock::now();
  {
    std::lock_guard context_lock(timers_.mutex);
    wake_worker = register_timer_locked(timer, now);
  }

  if (wake_worker) {
    wake_one_if_all_workers_sleeping();
  }
}

bool io_context::unregister_timer_locked(detail::timer_slot& timer) noexcept {
  // Caller must hold timers_.mutex.
  if (timer.context != this) {
    return false;
  }

  if (timer.active) {
    timers_.erase_heap(timer);
  } else {
    timers_.erase_inactive(timer);
  }
  timer.context = nullptr;
  const detail::timer_operation_queue canceled =
      take_timer_operations_locked(timer);
  enqueue_timer_operations_locked(canceled.head,
                                  detail::timer_completion_kind::canceled);
  return canceled.head != nullptr;
}

void io_context::unregister_timer(detail::timer_slot& timer) noexcept {
  bool wake_worker = false;
  {
    std::lock_guard context_lock(timers_.mutex);
    wake_worker = unregister_timer_locked(timer);
  }

  if (wake_worker) {
    wake_one_if_all_workers_sleeping();
  }
}

std::pair<std::size_t, bool> io_context::cancel_timer_locked(
    detail::timer_slot& timer) noexcept {
  // Caller must hold timers_.mutex. Returns the number of canceled waits
  // and whether a sleeping worker must be woken.
  if (timer.context != this) {
    return {0, false};
  }

  const detail::timer_operation_queue canceled =
      take_timer_operations_locked(timer);
  enqueue_timer_operations_locked(canceled.head,
                                  detail::timer_completion_kind::canceled);
  return {canceled.size, canceled.head != nullptr};
}

std::size_t io_context::cancel_timer(detail::timer_slot& timer) noexcept {
  std::pair<std::size_t, bool> result;
  {
    std::lock_guard context_lock(timers_.mutex);
    result = cancel_timer_locked(timer);
  }

  if (result.second) {
    wake_one_if_all_workers_sleeping();
  }
  return result.first;
}

std::pair<std::size_t, bool> io_context::set_timer_expiry_locked(
    detail::timer_slot& timer, time_point expiry, time_point now) noexcept {
  // Caller must hold timers_.mutex. Returns the number of canceled waits
  // and whether a sleeping worker must be woken.
  if (timer.context != this) {
    return {0, false};
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
  const bool has_canceled_operations = canceled.head != nullptr;
  enqueue_timer_operations_locked(canceled.head,
                                  detail::timer_completion_kind::canceled);

  if (timer.expiry > now) {
    timers_.push_heap(timer);
  } else {
    timers_.push_inactive(timer);
  }
  const bool wake_worker =
      has_canceled_operations || timers_.heap_deadline() < previous_deadline;
  return {canceled.size, wake_worker};
}

std::size_t io_context::set_timer_expiry(detail::timer_slot& timer,
                                         time_point expiry) noexcept {
  // Same rationale as register_timer: read the clock before the lock so the
  // per-op clock call is not inside the timers_.mutex critical section. A
  // stale now only delays, never advances, the expiry decision.
  const time_point now = clock::now();
  std::pair<std::size_t, bool> result;
  {
    std::lock_guard context_lock(timers_.mutex);
    result = set_timer_expiry_locked(timer, expiry, now);
  }

  if (result.second) {
    wake_one_if_all_workers_sleeping();
  }
  return result.first;
}

io_context::time_point io_context::timer_expiry(
    const detail::timer_slot& timer) const noexcept {
  std::lock_guard context_lock(timers_.mutex);
  return timer.expiry;
}

bool io_context::start_timer_wait_locked(
    detail::timer_operation_base& operation,
    detail::timer_slot& timer) noexcept {
  // Caller must hold timers_.mutex.
  if (timer.context != this) {
    operation.timer_next_ = nullptr;
    enqueue_timer_operations_locked(&operation,
                                    detail::timer_completion_kind::canceled);
    return true;
  }
  if (!timer.active) {
    operation.timer_next_ = nullptr;
    enqueue_timer_operations_locked(&operation,
                                    detail::timer_completion_kind::value);
    return true;
  }
  operation.timer_next_ = timer.submitted.head;
  timer.submitted.head = &operation;
  ++timer.submitted.size;
  return false;
}

void io_context::start_timer_wait(detail::timer_operation_base& operation,
                                  detail::timer_slot& timer) noexcept {
  bool wake_worker = false;
  {
    std::lock_guard context_lock(timers_.mutex);
    wake_worker = start_timer_wait_locked(operation, timer);
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
  // Note: the wake below is deliberately unconditional, unlike the *_locked
  // timer entry points which only wake when they staged work. A queued timer
  // completion must always reach the run loop, and switching this to a
  // conditional wake would require proving equivalence; the original
  // behavior is preserved.
  wake_one_if_all_workers_sleeping();
}

void io_context::drain_expired_timers_locked(time_point now) noexcept {
  // Caller must hold timers_.mutex. Moves every timer whose deadline has
  // passed out of the heap and enqueues its waits as value completions.
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
}

void io_context::reverse_ready_operations(
    detail::timer_operation_base* ready,
    detail::native_operation_base*& operations) noexcept {
  // Reverses the lock-protected ready list into the caller's output list so
  // the run loop executes the waits in submission order.
  while (ready != nullptr) {
    detail::timer_operation_base* const operation = ready;
    ready = operation->timer_next_;
    operation->timer_next_ = nullptr;
    operation->next = operations;
    operations = operation;
  }
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

  // Note: unlike register_timer/set_timer_expiry, the clock read stays inside
  // the critical section here. This path runs once per run-loop iteration
  // (amortized), and reading before the CAS gate would add a wasted clock
  // call on every CAS-loss / lock-busy retry under contention.
  const time_point now = clock::now();
  drain_expired_timers_locked(now);

  deadline = timers_.heap_deadline();
  detail::timer_operation_base* ready = timers_.ready;
  timers_.ready = nullptr;
  context_lock.unlock();
  timers_.timeout_fetching.store(false, std::memory_order_release);

  reverse_ready_operations(ready, operations);
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

void steady_timer::transfer_from(steady_timer& other) noexcept {
  // Unregister the other timer, carry over its expiry, and re-register this
  // slot with the same context so the moved-from timer leaves no registration
  // behind.
  io_context* context = other.timer_.context;
  if (context != nullptr) {
    const time_point expiry = context->timer_expiry(other.timer_);
    context->unregister_timer(other.timer_);
    timer_.expiry = expiry;
    context->register_timer(timer_);
  }
}

steady_timer::steady_timer(steady_timer&& other) noexcept {
  transfer_from(other);
}

steady_timer& steady_timer::operator=(steady_timer&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  if (timer_.context != nullptr) {
    timer_.context->unregister_timer(timer_);
  }

  transfer_from(other);
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
