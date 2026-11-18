/**
 * @file io_context_queue.cpp
 * @brief io_context I/O and CPU work queue operations and worker wakeup.
 */

#include <bnio/io_context.h>

#include <atomic>
#include <mutex>

namespace bnio {

detail::native_context* io_context::running_worker_native() const noexcept {
  // Nested run(): a worker native from another context must not receive the
  // operation (its local queue is never drained here); non-null implies alive.
  if (current_worker_native_ != nullptr &&
      current_worker_native_->get_global_state() == &global_state_) {
    return current_worker_native_;
  }
  return nullptr;
}

bool io_context::publish_io(operation_base& operation) noexcept {
  // Worker-local fast path: the publisher drains it, so no lock, wakeup, or
  // life_state check is needed; finish() completes leftovers as stopped.
  detail::native_context* worker = running_worker_native();
  if (worker != nullptr) {
    worker->publish_io(operation);
    return true;
  }
  return publish_io_deferred(operation);
}

bool io_context::publish_io_deferred(operation_base& operation) noexcept {
  // defer policy: never use the worker-local path, so I/O is not pinned to the
  // publisher. Under submit_lock: check state, enqueue, maybe wake.
  std::lock_guard<std::mutex> guard(global_state_.submit_lock);
  if (global_state_.life_state.load(std::memory_order_acquire) != 0) {
    return false;
  }
  global_state_.push_io(operation);
  wake_one_sleeping_locked();
  return true;
}

bool io_context::publish_cpu(
    detail::native_operation_base& operation) noexcept {
  // Worker-local fast path: the worker drains its local queue during finish(),
  // so the operation completes even during shutdown (see the bound check).
  detail::native_context* worker = running_worker_native();
  if (worker != nullptr) {
    worker->post(operation);
    return true;
  }
  return publish_cpu_deferred(operation);
}

bool io_context::publish_cpu_deferred(
    detail::native_operation_base& operation) noexcept {
  // defer policy: see publish_io_deferred(). Under submit_lock: if stopping,
  // do not enqueue and return false so the caller completes inline.
  std::lock_guard<std::mutex> guard(global_state_.submit_lock);
  if (global_state_.life_state.load(std::memory_order_acquire) != 0) {
    return false;
  }
  global_state_.push_cpu(operation);
  wake_one_sleeping_locked();
  return true;
}

void io_context::wake_locked() noexcept {
  // Caller holds submit_lock. Skip after the terminal state is published;
  // the destructor closes the channel under the same lock.
  if (global_state_.life_state.load(std::memory_order_acquire) != 0) {
    return;
  }
  (void)global_state_.wake_channel_.wake();
}

void io_context::wake_one_sleeping_locked() noexcept {
  // Caller holds submit_lock. Prefer a directed wake of one sleeping worker
  // over the shared broadcast; fall back when nobody is suspended.
  if (global_state_.life_state.load(std::memory_order_acquire) != 0) {
    return;
  }

  // Advisory racy check: if every worker that entered run() is still awake,
  // nobody is suspended; a false negative just delays the wake by one pass.
  if (global_state_.awake_workers.load(std::memory_order_acquire) >=
      global_state_.running_workers.load(std::memory_order_acquire)) {
    return;
  }

  if (!global_state_.wake_one_sleeping()) {
    wake_locked();
  }
}

void io_context::wake_one_if_all_workers_sleeping() noexcept {
  std::lock_guard<std::mutex> guard(global_state_.submit_lock);
  // A blocked worker re-arms at its deadline, so one wake suffices when all
  // sleep; timer entries stage this when the deadline moves earlier or a wait
  // is canceled or completed.
  if (global_state_.awake_workers.load(std::memory_order_acquire) != 0) {
    return;
  }
  wake_one_sleeping_locked();
}

}  // namespace bnio
