#include <bnio/linux/io_context.h>

#include <atomic>
#include <cstddef>

#include "bnio/async_io/linux/io_uring_context_base/context.h"
#include "bnio/async_io/linux/io_uring_context_base/operation_base.h"
#include "bnio/linux/detail/io_context_state.h"

namespace bnio {

using detail::native_worker;

void io_context::publish_io(operation_base& operation) noexcept {
  global_state_.push_io(operation);
  wake_one_worker();
}

void io_context::publish_cpu(
    async_io::linux_native::io_uring_operation_base& operation) noexcept {
  global_state_.push_cpu(operation);
  wake_one_worker();
}

void io_context::wake_one_worker() noexcept {
  native_worker* worker = native_workers_.head.load(std::memory_order_acquire);
  while (worker != nullptr) {
    if (worker->context.is_waiting()) {
      worker->context.notify_one_waiter();
      return;
    }
    worker = worker->next;
  }
}

void io_context::wake_one_if_all_workers_sleeping() noexcept {
  if (global_state_.awake_workers.load(std::memory_order_acquire) != 0) {
    return;
  }
  wake_one_worker();
}

}  // namespace bnio
