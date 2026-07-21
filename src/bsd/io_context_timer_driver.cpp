#include <bnio/bsd/io_context.h>

#include <atomic>
#include <cstddef>
#include <mutex>

#include "bnio/async_io/bsd/kqueue_context_base/context.h"
#include "bnio/bsd/detail/io_context_timer_types.h"

namespace bnio {

void io_context::on_timer_driver() noexcept {
  {
    std::lock_guard context_lock(timers_.mutex);
    timers_.complete_driver();
  }
  wake_one_worker();
}

bool io_context::try_fetch_timeout_operations(time_point& deadline,
                                              bool& fetched) noexcept {
  std::unique_lock context_lock(timers_.mutex, std::try_to_lock);
  if (!context_lock.owns_lock()) {
    return false;
  }

  detail::timer_operation_queue ready;
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

  deadline = timers_.heap_deadline();
  fetched = ready.head != nullptr;
  context_lock.unlock();
  post_timer_operations(ready.head, detail::timer_completion_kind::value);
  return true;
}

bool io_context::try_fetch_timeout_operations_thunk(void* state,
                                                    time_point& deadline,
                                                    bool& fetched) noexcept {
  auto* timers = static_cast<detail::timer_state_data*>(state);
  if (timers == nullptr) {
    return false;
  }
  return timers->owner != nullptr
             ? timers->owner->try_fetch_timeout_operations(deadline, fetched)
             : false;
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

}  // namespace bnio
