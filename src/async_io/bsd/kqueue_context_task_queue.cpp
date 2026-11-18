/**
 * @file kqueue_context_task_queue.cpp
 * @brief Task queue operations: post, publish, CPU push, wait/notify.
 */

#include <cassert>
#include <cerrno>
#include <mutex>

#include "kqueue_context_internal.h"

namespace bnio::async_io::bsd_native {

int kqueue_context::post(kqueue_operation_base& operation) noexcept {
  assert_running();

  if (current_context_ == this || global_state_ == nullptr) {
    assert(!run_state_.run_active.load(std::memory_order_acquire) ||
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

  // Worker-local fast path: a callback on this worker publishes to its own
  // queue with no lock or wakeup, keeping it on the owning worker.
  if (current_context_ == this || global_state_ == nullptr) {
    local_state_.push_io(operation);
    return;
  }

  global_state_->push_io(operation);
  notify_one_waiter();
}

void kqueue_context::push_cpu_tasks(operation_queue& operations) noexcept {
  if (global_state_ == nullptr) {
    local_state_.push_cpu(operations.pop_all());
    return;
  }
  kqueue_operation_base* ordered_tasks = operations.pop_all();
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

kqueue_operation_base* kqueue_context::fetch_cpu_task() noexcept {
  // Local queue first (locality), then shared.
  if (kqueue_operation_base* operations = local_state_.pop_cpu_all()) {
    return operations;
  }

  if (global_state_ == nullptr) {
    return nullptr;
  }

  if (kqueue_operation_base* operations = global_state_->pop_cpu_all()) {
    return operations;
  }

  return nullptr;
}

bool kqueue_context::run_cpu_batch() noexcept {
  if (kqueue_operation_base* operations = fetch_cpu_task()) {
    execute_tasks(operations);
    return true;
  }
  return false;
}

void kqueue_context::unregister_local_state() noexcept {
  if (global_state_ == nullptr) {
    return;
  }
  // Unlink from the suspend list if still linked; the normal wait path
  // already unlinked in end_wait().
  kqueue_worker_state_list& suspend = global_state_->workers;
  std::lock_guard<std::mutex> guard(suspend.lock);
  if (kqueue_local_state_in_list(suspend, &local_state_)) {
    kqueue_unlink_local_state(suspend, &local_state_);
  }
}

void kqueue_context::notify_one_waiter() noexcept {
  if (is_waiting()) {
    (void)trigger_wakeup();
  }
}

bool kqueue_context::is_waiting() const noexcept {
  return run_state_.waiting.load(std::memory_order_acquire);
}

void kqueue_context::begin_wait() noexcept {
  run_state_.waiting.store(true, std::memory_order_release);
  if (global_state_ == nullptr) {
    return;
  }
  // Link into the suspend list so a publisher can find and wake this worker.
  {
    kqueue_worker_state_list& suspend = global_state_->workers;
    std::lock_guard<std::mutex> guard(suspend.lock);
    kqueue_link_local_state(suspend, &local_state_);
  }
  [[maybe_unused]] const std::size_t previous =
      global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
  assert(previous != 0);
}

void kqueue_context::end_wait() noexcept {
  if (global_state_ != nullptr) {
    // Unlink from the suspend list; the worker is awake again.
    {
      kqueue_worker_state_list& suspend = global_state_->workers;
      std::lock_guard<std::mutex> guard(suspend.lock);
      kqueue_unlink_local_state(suspend, &local_state_);
    }
    global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  }
  run_state_.waiting.store(false, std::memory_order_release);
}

int kqueue_context::trigger_wakeup() noexcept {
  // Shared wake channel when available; otherwise trigger this context's
  // EVFILT_USER.
  if (global_state_ != nullptr && global_state_->wake_channel_.is_open()) {
    // Hold submit_lock so the destructor's close cannot race the write;
    // re-check after acquiring it, the channel may already be closed.
    std::lock_guard guard(global_state_->submit_lock);
    if (!global_state_->wake_channel_.is_open()) {
      return -EBADF;
    }
    return global_state_->wake_channel_.wake();
  }

  // Standalone mode: trigger own kqueue's EVFILT_USER.
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

void* kqueue_context::local_wakeup_user_data() noexcept {
  static int local_wakeup_sentinel = 0;
  return &local_wakeup_sentinel;
}

}  // namespace bnio::async_io::bsd_native
