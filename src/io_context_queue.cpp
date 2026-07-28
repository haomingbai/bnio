/**
 * @file io_context_queue.cpp
 * @brief io_context I/O and CPU work queue operations and worker wakeup.
 */

#include <bnio/io_context.h>

#include <atomic>
#include <cstddef>

namespace bnio {

void io_context::publish_io(operation_base& operation) noexcept {
  // Worker-local fast path: publish directly to the running native
  // context's local queue to avoid CAS on the shared MPSC queue and
  // prevent descriptor/connection migration to another kqueue.
  if (current_worker_native_ != nullptr) {
    current_worker_native_->publish_io(operation);
    return;
  }
  global_state_.push_io(operation);
  wake_one_if_all_workers_sleeping();
}

void io_context::publish_cpu(
    detail::native_operation_base& operation) noexcept {
  // Worker-local fast path: post directly to the running native
  // context's local queue to avoid CAS on the shared MPSC queue.
  if (current_worker_native_ != nullptr) {
    current_worker_native_->post(operation);
    return;
  }
  global_state_.push_cpu(operation);
  wake_one_if_all_workers_sleeping();
}

void io_context::wake_one_worker() noexcept {
  // Write to the shared wake channel. A single write wakes all workers
  // whose native contexts have read interest registered on the channel
  // (minor thundering herd). Extra workers that wake up only perform
  // one read→EAGAIN plus one pop_cpu_all() CAS before returning to
  // sleep — negligible for typical 4–8 worker concurrency.
  (void)global_state_.wake_channel_.wake();
}

void io_context::wake_one_if_all_workers_sleeping() noexcept {
  if (global_state_.awake_workers.load(std::memory_order_acquire) != 0) {
    return;
  }
  wake_one_worker();
}

}  // namespace bnio
