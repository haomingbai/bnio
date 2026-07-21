#include <bnio/linux/io_context.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>

#include "bnio/async_io/linux/io_uring_context_base/context.h"
#include "bnio/linux/detail/io_context_timer_types.h"

namespace bnio {

void io_context::on_timer_wakeup() noexcept {
  {
    std::lock_guard context_lock(timers_.mutex);
    timers_.complete_wakeup();
  }
  post_timer_driver();
}

void io_context::on_timer_update() noexcept {
  std::lock_guard context_lock(timers_.mutex);
  timers_.complete_update();
  schedule_timer_wakeup_locked();
}

void io_context::on_timer_driver() noexcept {
  detail::timer_operation_queue ready;

  {
    std::lock_guard context_lock(timers_.mutex);
    timers_.complete_driver();

    const time_point now = clock::now();
    while (timers_.heap_deadline() <= now) {
      detail::timer_slot* const timer = timers_.pop_heap();
      if (timer == nullptr) {
        break;
      }

      const detail::timer_operation_queue operations =
          take_timer_operations_locked(*timer);
      timers_.push_inactive(*timer);
      if (operations.head != nullptr) {
        if (ready.tail != nullptr) {
          ready.tail->timer_next_ = operations.head;
        } else {
          ready.head = operations.head;
        }
        ready.tail = operations.tail;
      }
    }

    schedule_timer_wakeup_locked();
  }

  post_timer_operations(ready.head, detail::timer_completion_kind::value);
}

void io_context::post_timer_driver() noexcept {
  if (global_state_.closing.load(std::memory_order_acquire)) {
    return;
  }
  if (timers_.queue_driver()) {
    (void)primary_native_context().post(timer_driver_operation_);
  }
}

detail::timer_operation_queue io_context::take_timer_operations_locked(
    detail::timer_slot& timer) noexcept {
  const detail::timer_operation_queue operations = timer.submitted;
  timer.submitted = {};
  return operations;
}

void io_context::post_timer_operations(
    detail::timer_operation_base* operations,
    detail::timer_completion_kind completion) noexcept {
  if (global_state_.closing.load(std::memory_order_acquire)) {
    return;
  }

  while (operations != nullptr) {
    detail::timer_operation_base* const operation = operations;
    operations = operations->timer_next_;
    operation->timer_next_ = nullptr;
    operation->timer_completion_ = completion;
    (void)primary_native_context().post(*operation);
  }
}

void io_context::schedule_timer_wakeup_locked() noexcept {
  const time_point deadline = timers_.heap_deadline();
  if (deadline == time_point::max()) {
    return;
  }

  if (timers_.can_queue_wakeup()) {
    queue_timer_wakeup_locked(deadline);
    return;
  }

  if (timers_.can_queue_update(deadline)) {
    queue_timer_update_locked(deadline);
  }
}

void io_context::queue_timer_wakeup_locked(time_point deadline) noexcept {
  if (!timers_.can_queue_wakeup()) {
    return;
  }

  timer_wakeup_operation_.set_timeout(
      std::max(deadline - clock::now(), duration::zero()));

  primary_native_context().publish_io(timer_wakeup_operation_);
  timers_.mark_wakeup_queued(deadline);
}

void io_context::queue_timer_update_locked(time_point deadline) noexcept {
  if (!timers_.can_queue_update(deadline)) {
    return;
  }

  timer_update_operation_.set_timeout(
      std::max(deadline - clock::now(), duration::zero()));

  primary_native_context().publish_io(timer_update_operation_);
  timers_.mark_update_queued(deadline);
}

}  // namespace bnio
