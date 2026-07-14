#include <bupp/linux/io_context.h>

#include <atomic>
#include <cstddef>

#include "bupp/async_io/linux/io_uring_context_base/context.h"
#include "bupp/async_io/linux/io_uring_context_base/operation_base.h"
#include "bupp/linux/detail/io_context_state.h"

namespace bupp {

using detail::native_worker;

void io_context::publish_io(operation_base& operation) noexcept {
  global_state_.push_io(operation);
  wake_one_worker();
}

void io_context::post(
    async_io::linux_native::io_uring_operation_base& operation) noexcept {
  native_worker& worker = select_worker();
  async_io::linux_native::io_uring_context* native_context =
      worker.context.load(std::memory_order_acquire);
  if (native_context != nullptr) {
    (void)native_context->post(operation);
    wake_one_worker();
  }
}

void io_context::wake_one_worker() noexcept {
  const std::size_t worker_count =
      native_workers_.active_count.load(std::memory_order_acquire);
  if (global_state_.awake_workers.load(std::memory_order_acquire) >=
      worker_count) {
    return;
  }

  native_worker* worker = native_workers_.head;
  for (std::size_t index = 0; index < worker_count && worker != nullptr;
       ++index) {
    auto* native_context = worker->context.load(std::memory_order_acquire);
    if (native_context != nullptr && native_context->is_waiting()) {
      native_context->notify_one_waiter();
      return;
    }
    worker = worker->next.load(std::memory_order_acquire);
  }
}

}  // namespace bupp
