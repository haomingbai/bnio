#include <bupp/bsd/io_context.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bupp/async_io/bsd/kqueue_context_base/context.h"
#include "bupp/bsd/detail/io_context_timer_types.h"

namespace bupp {

void io_context::on_timer_driver() noexcept {
  bool wake_waiter = false;
  {
    std::lock_guard context_lock(timers_.mutex);
    timers_.complete_driver();
    const time_point old_deadline = timers_.heap.empty()
                                        ? time_point::max()
                                        : timers_.heap.front().deadline;

    for (auto& entry : timers_.timers) {
      detail::timer_slot* timer = entry.second;
      if (timer == nullptr) {
        continue;
      }
      (void)drain_timer_submissions_locked(*timer);
    }
    const time_point new_deadline = timers_.heap.empty()
                                        ? time_point::max()
                                        : timers_.heap.front().deadline;
    if (new_deadline < old_deadline) {
      wake_waiter = true;
    }
  }
  if (wake_waiter) {
    wake_one_worker();
  }
}

bool io_context::try_fetch_timeout_operations(time_point& deadline,
                                              bool& fetched) noexcept {
  std::unique_lock context_lock(timers_.mutex, std::try_to_lock);
  if (!context_lock.owns_lock()) {
    return false;
  }
  detail::timer_operation_base* ready = nullptr;
  const time_point now = clock::now();
  while (!timers_.heap.empty() && timers_.heap.front().deadline <= now) {
    const detail::timer_heap_item item = timers_.heap.front();
    timers_.pop_heap();

    auto iterator = timers_.timers.find(item.timer_id);
    if (iterator == timers_.timers.end() || iterator->second == nullptr) {
      continue;
    }

    detail::timer_slot* timer = iterator->second;
    std::lock_guard timer_lock(timer->mutex);
    if (timer->context != this || timer->generation != item.generation) {
      continue;
    }

    detail::timer_operation_base* operations = timer->waiting_head;
    timer->waiting_head = nullptr;
    if (operations != nullptr) {
      operations = reverse_timer_operations(operations);
      detail::timer_operation_base* tail = operations;
      while (tail->timer_next_ != nullptr) {
        tail = tail->timer_next_;
      }
      tail->timer_next_ = ready;
      ready = operations;
    }
  }

  deadline =
      timers_.heap.empty() ? time_point::max() : timers_.heap.front().deadline;
  fetched = ready != nullptr;
  context_lock.unlock();
  post_timer_operations(ready, detail::timer_completion_kind::value);
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
  bool should_post = false;
  {
    std::lock_guard context_lock(timers_.mutex);
    if (timers_.queue_driver()) {
      should_post = true;
    }
  }

  if (should_post) {
    (void)primary_native_context().post(timer_driver_operation_);
  }
}

std::size_t io_context::drain_timer_submissions_locked(
    detail::timer_slot& timer) noexcept {
  std::lock_guard timer_lock(timer.mutex);
  if (timer.context != this) {
    return 0;
  }

  detail::timer_operation_base* submitted =
      timer.submitted_head.exchange(nullptr, std::memory_order_acq_rel);
  if (submitted == nullptr) {
    return 0;
  }

  submitted = reverse_timer_operations(submitted);
  const std::size_t count = count_timer_operations(submitted);

  detail::timer_operation_base* tail = submitted;
  while (tail->timer_next_ != nullptr) {
    tail = tail->timer_next_;
  }
  tail->timer_next_ = timer.waiting_head;
  timer.waiting_head = submitted;
  timers_.push_heap(detail::timer_heap_item{
      .deadline = timer.expiry,
      .timer_id = timer.id,
      .generation = timer.generation,
  });

  return count;
}

detail::timer_operation_base* io_context::take_timer_waiters_locked(
    detail::timer_slot& timer) noexcept {
  detail::timer_operation_base* submitted =
      timer.submitted_head.exchange(nullptr, std::memory_order_acq_rel);
  detail::timer_operation_base* waiting = timer.waiting_head;
  timer.waiting_head = nullptr;

  if (submitted == nullptr) {
    return waiting;
  }

  submitted = reverse_timer_operations(submitted);
  detail::timer_operation_base* tail = submitted;
  while (tail->timer_next_ != nullptr) {
    tail = tail->timer_next_;
  }
  tail->timer_next_ = waiting;
  return submitted;
}

void io_context::push_timer_operation(
    std::atomic<detail::timer_operation_base*>& head,
    detail::timer_operation_base& operation) noexcept {
  detail::timer_operation_base* current_head =
      head.load(std::memory_order_acquire);
  do {
    operation.timer_next_ = current_head;
  } while (!head.compare_exchange_weak(current_head, &operation,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire));
}

detail::timer_operation_base* io_context::reverse_timer_operations(
    detail::timer_operation_base* operations) noexcept {
  detail::timer_operation_base* reversed = nullptr;
  while (operations != nullptr) {
    detail::timer_operation_base* operation = operations;
    operations = operations->timer_next_;
    operation->timer_next_ = reversed;
    reversed = operation;
  }
  return reversed;
}

std::size_t io_context::count_timer_operations(
    detail::timer_operation_base* operations) noexcept {
  std::size_t count = 0;
  while (operations != nullptr) {
    ++count;
    operations = operations->timer_next_;
  }
  return count;
}

void io_context::post_timer_operations(
    detail::timer_operation_base* operations,
    detail::timer_completion_kind completion) noexcept {
  while (operations != nullptr) {
    detail::timer_operation_base* operation = operations;
    operations = operations->timer_next_;
    operation->timer_next_ = nullptr;
    operation->timer_completion_ = completion;
    (void)primary_native_context().post(*operation);
  }
}

}  // namespace bupp
