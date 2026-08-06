/**
 * @file io_context_queue.cpp
 * @brief io_context I/O and CPU work queue operations and worker wakeup.
 */

#include <bnio/io_context.h>

#include <atomic>
#include <mutex>

namespace bnio {

bool io_context::publish_io(operation_base& operation) noexcept {
  // Experimental: worker-local fast path removed so all I/O publications
  // contend on the global queue, breaking connection affinity.
  //
  // Critical section, ordered against begin_stop() / ~io_context() by the
  // same submit_lock. Contains only state-involving work: check the shutdown
  // state, enqueue, and decide whether a sleeping worker must be woken.
  // The caller executes the operation afterwards.
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
  // Worker-local fast path: post directly to the running native context's
  // local queue. The worker always drains its local queue during finish(),
  // so the operation is guaranteed to complete even during shutdown.
  if (current_worker_native_ != nullptr) {
    current_worker_native_->post(operation);
    return true;
  }

  // Critical section, ordered against begin_stop() / ~io_context() by the
  // same submit_lock. The critical section contains ONLY state-involving
  // work:
  //   1. check the shutdown state,
  //   2. publish (enqueue) — the queue head is shared state, and the
  //      enqueue must be atomic with the check above,
  //   3. decide whether a sleeping worker must be woken (reads awake_workers).
  // Operation execution (complete/execute) never happens here; the caller
  // runs it after publish_cpu returns.
  //
  // publish_cpu publishes an operation ONLY while the context is not
  // stopping; it assumes the publish happens against a running context.
  // When the context is already stopping it does NOT enqueue (the queue may
  // no longer be drained) and returns false — the caller must then complete
  // the operation inline so it never strands.
  std::lock_guard<std::mutex> guard(global_state_.submit_lock);
  if (global_state_.life_state.load(std::memory_order_acquire) != 0) {
    return false;
  }
  global_state_.push_cpu(operation);
  wake_one_sleeping_locked();
  return true;
}

void io_context::wake_locked() noexcept {
  // Caller must hold global_state_.submit_lock. The shutdown-state check
  // keeps the wake bound to the destructor's close: ~io_context() publishes
  // the terminal state and closes the channel under the same lock, so once
  // this sees the terminal state the channel is (or is about to be) closed
  // and writing would touch a closed fd.
  if (global_state_.life_state.load(std::memory_order_acquire) != 0) {
    return;
  }
  (void)global_state_.wake_channel_.wake();
}

void io_context::wake_one_sleeping_locked() noexcept {
  // Caller must hold global_state_.submit_lock. Prefer a directed wake of
  // exactly one sleeping worker over the shared broadcast channel: each
  // worker listens to both its per-worker wake channel and the shared one,
  // so a single write reaches only the intended worker (no thundering
  // herd). Falls back to the shared channel when nobody is suspended.
  if (global_state_.life_state.load(std::memory_order_acquire) != 0) {
    return;
  }
  if (!global_state_.wake_one_sleeping()) {
    wake_locked();
  }
}

void io_context::wake_one_if_all_workers_sleeping() noexcept {
  std::lock_guard<std::mutex> guard(global_state_.submit_lock);
  // Timer deadlines are delivered by each worker's blocking timeout: a worker
  // blocked in io_uring_enter with a timeout wakes itself when its deadline
  // arrives. So a new or earlier timer deadline only needs to wake a worker
  // when every worker is already blocked (their armed timeouts would fire too
  // late); otherwise the awake worker observes the new nearest deadline on its
  // next run-loop pass and re-arms its own timeout. The timer entry points
  // stage this wake only when the heap deadline moved earlier (or a wait was
  // canceled), so reaching here with all workers sleeping implies a genuine
  // re-arm is needed. Waking exactly one sleeping worker is sufficient: it
  // re-arms at the earlier deadline and dispatches it when it fires.
  if (global_state_.awake_workers.load(std::memory_order_acquire) != 0) {
    return;
  }
  wake_one_sleeping_locked();
}

}  // namespace bnio
