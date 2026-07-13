#include <bupp/linux/io_context.h>

#include <cerrno>

namespace bupp {

using detail::native_worker;

void io_context::enqueue_io(operation_base& operation) noexcept {
  task_queue_.push_io(operation);
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

bool io_context::drain_pending_io(
    void* owner_data,
    async_io::linux_native::io_uring_context& native_context) noexcept {
  auto& owner = *static_cast<io_context*>(owner_data);

  operation_base* operations = owner.take_pending_io();
  if (operations == nullptr) {
    return false;
  }

  owner.submit_operations(operations, native_context);
  return true;
}

// Fill SQEs from chain into the io_uring ring until the ring is full or the
// chain is exhausted.  Operations that fail preparation (non-EAGAIN errors)
// are detached from the chain and appended to **failed_tail via a
// double-pointer.
//
// Returns the first operation that could not be prepared (-EAGAIN), or nullptr
// if the entire chain was consumed.
static io_context::operation_base* try_prepare_batch(
    io_context::operation_base* chain, const auto& prepare,
    io_context::operation_base**& failed_tail) noexcept {
  while (chain != nullptr) {
    io_context::operation_base* next = chain->pending_next;
    chain->pending_next = nullptr;

    const int result = prepare(*chain);

    if (result == -EAGAIN) {
      chain->pending_next = next;  // restore link, caller will retry
      return chain;
    }

    if (result < 0) {
      chain->complete_submit_error(result);
      *failed_tail = chain;
      failed_tail = &chain->pending_next;
      chain = next;
      continue;
    }

    // SQE prepared successfully — operation is now owned by the ring.
    chain = next;
  }

  return nullptr;
}

void io_context::submit_operations(
    operation_base* operations,
    async_io::linux_native::io_uring_context& native_context) noexcept {
  operation_base* failed_head = nullptr;
  operation_base** failed_tail = &failed_head;

  native_context.submit_batch([&](auto prepare, auto submit) noexcept {
    while (operations != nullptr) {
      operations = try_prepare_batch(operations, prepare, failed_tail);
      (void)submit();
    }
  });

  while (failed_head != nullptr) {
    operation_base* op = failed_head;
    failed_head = failed_head->pending_next;
    op->pending_next = nullptr;
    (void)native_context.post(*op);
  }
}

io_context::operation_base* io_context::take_pending_io() noexcept {
  auto* incoming = task_queue_.pop_io_all();
  operation_base* operations = nullptr;
  operation_base** tail = &operations;
  while (incoming != nullptr) {
    auto* operation = static_cast<operation_base*>(incoming);
    incoming = incoming->next;
    operation->next = nullptr;
    operation->pending_next = nullptr;
    *tail = operation;
    tail = &operation->pending_next;
  }
  return operations;
}

void io_context::wake_one_worker() noexcept {
  const std::size_t worker_count =
      native_workers_.active_count.load(std::memory_order_acquire);
  if (task_queue_.awake_workers.load(std::memory_order_acquire) >=
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
