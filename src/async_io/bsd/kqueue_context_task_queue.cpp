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

  // Experimental: worker-local fast path removed so all I/O publications
  // contend on the global queue, breaking connection affinity.
  if (global_state_ != nullptr) {
    global_state_->push_io(operation);
    notify_one_waiter();
    return;
  }

  // Standalone mode (no shared state): keep a local IO queue drained by
  // consume_io_tasks(). IO stealing is intentionally unsupported.
  operation.io_next = local_io_head_;
  local_io_head_ = &operation;
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

kqueue_operation_base* kqueue_context::fetch_cpu_task() noexcept {
  // 1. Worker-local queue first: fastest and preserves locality.
  if (kqueue_operation_base* operations = local_state_.pop_cpu_all()) {
    return reverse_tasks(operations);
  }

  if (global_state_ == nullptr) {
    return nullptr;
  }

  // 2. Shared CPU queue.
  if (kqueue_operation_base* operations = global_state_->pop_cpu_all()) {
    return reverse_tasks(operations);
  }

  // 3. Steal from another worker's local queue. Only reached when both
  //    local and shared queues are empty, so a stealing worker is by
  //    definition relatively idle.
  return steal_cpu_tasks();
}

bool kqueue_context::run_cpu_batch() noexcept {
  if (kqueue_operation_base* operations = fetch_cpu_task()) {
    execute_tasks(operations);
    return true;
  }
  return false;
}

kqueue_operation_base* kqueue_context::steal_cpu_tasks() noexcept {
  if (global_state_ == nullptr) {
    return nullptr;
  }

  // Conservative gate: only attempt a steal while MORE workers are active
  // than suspended (active > suspend  <=>  2*active > running). A worker
  // suspends only after finding no work, so when the majority are suspended
  // the remaining active workers' local queues are likely empty too, and
  // scanning the run list would just pay run.lock contention for nothing.
  // The two loads are deliberately not coordinated — this is a racy,
  // relaxed heuristic that only decides whether the scan is worth the lock;
  // the actual steal below stays correct regardless of the gate's accuracy.
  const std::size_t active =
      global_state_->awake_workers.load(std::memory_order_relaxed);
  const std::size_t running =
      global_state_->running_workers.load(std::memory_order_relaxed);
  if (active * 2 <= running) {
    return nullptr;
  }

  // The lock guards both the list traversal and the lifetime of every node:
  // a worker unregisters its local state under the same lock before its
  // context (and local state) is destroyed, so a visited node is never freed
  // while we touch it (UAF protection). Only running workers can hold CPU
  // tasks worth stealing, so we traverse the run list.
  kqueue_worker_state_list& run = global_state_->workers.run;
  std::lock_guard<std::mutex> guard(run.lock);

  // Fairness via shared list rotation: every stealer probes from the run
  // list head and moves each probed node to the tail, so the head keeps
  // advancing globally and all workers share one rotating scan order — no
  // per-thread cursor to keep in sync. Wrap after one full circuit.
  //
  // Each probe is a relaxed load first: only pay for the locked-RMW exchange
  // (pop_cpu_all) on a node that actually shows work, so a doomed scan costs
  // n loads + n O(1) rotations instead of n exchanges. This must happen under
  // run.lock — a lock-free traversal could dereference a node whose owner
  // unregisters (and whose context is destroyed) concurrently.
  kqueue_local_task_queue_state* node = run.head;
  const kqueue_local_task_queue_state* const start = run.head;
  while (node != nullptr) {
    kqueue_local_task_queue_state* const next = node->next;
    if (node != &local_state_ && node->has_cpu_tasks()) {
      if (kqueue_operation_base* operations = node->pop_cpu_all()) {
        kqueue_rotate_local_state_to_tail(run, node);
        return reverse_tasks(operations);
      }
    }
    kqueue_rotate_local_state_to_tail(run, node);
    if (next == start) {
      break;
    }
    node = next;
  }

  return nullptr;
}

void kqueue_context::register_local_state() noexcept {
  if (global_state_ == nullptr) {
    return;
  }
  kqueue_worker_state_list& run = global_state_->workers.run;
  std::lock_guard<std::mutex> guard(run.lock);
  kqueue_link_local_state(run, &local_state_);
}

void kqueue_context::unregister_local_state() noexcept {
  if (global_state_ == nullptr) {
    return;
  }
  // Unlink from whichever list the worker currently resides in. Only one
  // list is touched here; a worker is never on both at once.
  {
    kqueue_worker_state_list& run = global_state_->workers.run;
    std::lock_guard<std::mutex> guard(run.lock);
    if (kqueue_local_state_in_list(run, &local_state_)) {
      kqueue_unlink_local_state(run, &local_state_);
      return;
    }
  }
  {
    kqueue_worker_state_list& suspend = global_state_->workers.suspend;
    std::lock_guard<std::mutex> guard(suspend.lock);
    if (kqueue_local_state_in_list(suspend, &local_state_)) {
      kqueue_unlink_local_state(suspend, &local_state_);
    }
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
  // Move from the run list to the suspend list. The two list locks are
  // taken separately (never nested) to avoid deadlock between a waking
  // and a sleeping worker.
  {
    kqueue_worker_state_list& run = global_state_->workers.run;
    std::lock_guard<std::mutex> guard(run.lock);
    kqueue_unlink_local_state(run, &local_state_);
  }
  {
    kqueue_worker_state_list& suspend = global_state_->workers.suspend;
    std::lock_guard<std::mutex> guard(suspend.lock);
    kqueue_link_local_state(suspend, &local_state_);
  }
  [[maybe_unused]] const std::size_t previous =
      global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
  assert(previous != 0);
}

void kqueue_context::end_wait() noexcept {
  if (global_state_ != nullptr) {
    // Move back from the suspend list to the run list, again without
    // nesting the two list locks.
    {
      kqueue_worker_state_list& suspend = global_state_->workers.suspend;
      std::lock_guard<std::mutex> guard(suspend.lock);
      kqueue_unlink_local_state(suspend, &local_state_);
    }
    {
      kqueue_worker_state_list& run = global_state_->workers.run;
      std::lock_guard<std::mutex> guard(run.lock);
      kqueue_link_local_state(run, &local_state_);
    }
    global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  }
  run_state_.waiting.store(false, std::memory_order_release);
}

int kqueue_context::trigger_wakeup() noexcept {
  // Use the shared wake channel when available (multi-worker mode).
  // Fall back to per-context EVFILT_USER NOTE_TRIGGER for legacy
  // standalone operation (no global state).
  if (global_state_ != nullptr && global_state_->wake_channel_.is_open()) {
    // Bind the shared wake-channel write to the submit lock so the
    // io_context destructor's close (under the same lock) can never race
    // it. Re-check after acquiring the lock: the channel may have been
    // closed while we waited for the lock.
    std::lock_guard guard(global_state_->submit_lock);
    if (!global_state_->wake_channel_.is_open()) {
      return -EBADF;
    }
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

void* kqueue_context::local_wakeup_user_data() noexcept {
  static int local_wakeup_sentinel = 0;
  return &local_wakeup_sentinel;
}

}  // namespace bnio::async_io::bsd_native
