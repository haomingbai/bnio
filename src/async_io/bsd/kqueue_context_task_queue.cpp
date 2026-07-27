/**
 * @file kqueue_context_task_queue.cpp
 * @brief Task queue operations: post, publish, CPU push, wait/notify.
 */

#include <cassert>
#include <cerrno>

#include "kqueue_context_internal.h"

namespace bnio::async_io::bsd_native {

int kqueue_context::post(kqueue_operation_base& operation) noexcept {
  assert_running();

  if (current_context_ == this || global_state_ == nullptr) {
    assert(!run_active_.load(std::memory_order_acquire) ||
           current_context_ == this);
    local_state_.push_cpu(operation);
    return 0;
  }

  global_state_->push_cpu(operation);
  notify_one_waiter();
  return 0;
}

void kqueue_context::publish_io(kqueue_io_operation_base& operation) noexcept {
  assert_running();
  if (global_state_ != nullptr) {
    global_state_->push_io(operation);
    notify_one_waiter();
    return;
  }

  assert(!run_active_.load(std::memory_order_acquire) ||
         current_context_ == this);
  local_state_.push_io(operation);
}

void kqueue_context::push_cpu_tasks(operation_queue& operations) noexcept {
  if (global_state_ == nullptr) {
    local_state_.push_cpu(reverse_tasks(operations.pop_all()));
    return;
  }
  kqueue_operation_base* ordered_tasks = reverse_tasks(operations.pop_all());
  if (ordered_tasks == nullptr) {
    return;
  }

  while (ordered_tasks != nullptr) {
    kqueue_operation_base* operation = ordered_tasks;
    ordered_tasks = ordered_tasks->next;
    operation->next = nullptr;
    global_state_->push_cpu(*operation);
  }
  notify_one_waiter();
}

bool kqueue_context::consume_global_state() noexcept {
  if (global_state_ == nullptr) {
    return false;
  }

  bool consumed = false;
  if (kqueue_operation_base* incoming = global_state_->pop_cpu_all()) {
    local_state_.push_cpu(reverse_tasks(incoming));
    consumed = true;
  }
  if (kqueue_io_operation_base* incoming = global_state_->pop_io_all()) {
    kqueue_io_operation_base** tail = &incoming;
    while (*tail != nullptr) {
      tail = &(*tail)->io_next;
    }
    *tail = incoming_io_tasks_;
    incoming_io_tasks_ = incoming;
    consumed = true;
  }
  return consumed;
}

void kqueue_context::notify_one_waiter() noexcept {
  if (is_waiting()) {
    (void)trigger_wakeup();
  }
}

bool kqueue_context::is_waiting() const noexcept {
  return waiting_.load(std::memory_order_acquire);
}

void kqueue_context::begin_wait() noexcept {
  waiting_.store(true, std::memory_order_release);
  if (global_state_ == nullptr) {
    return;
  }
  [[maybe_unused]] const std::size_t previous =
      global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
  assert(previous != 0);
}

void kqueue_context::end_wait() noexcept {
  if (global_state_ != nullptr) {
    global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  }
  waiting_.store(false, std::memory_order_release);
}

int kqueue_context::trigger_wakeup() noexcept {
  // Use the shared wake channel when available (multi-worker mode).
  // Fall back to per-context EVFILT_USER NOTE_TRIGGER for legacy
  // standalone operation (no global state).
  if (global_state_ != nullptr && global_state_->wake_channel_.is_open()) {
    return global_state_->wake_channel_.wake();
  }

  // Legacy standalone mode: trigger own kqueue's EVFILT_USER.
  if (!queue_.is_open()) {
    return -EINVAL;
  }
  bnio::base::event trigger(options_.wakeup_ident, EVFILT_USER, 0, NOTE_TRIGGER,
                            0, wakeup_user_data());
  return queue_.control(&trigger, 1, nullptr, 0, nullptr);
}

void* kqueue_context::wakeup_user_data() noexcept {
  static int wakeup_sentinel = 0;
  return &wakeup_sentinel;
}

}  // namespace bnio::async_io::bsd_native
