#include <bupp/linux/io_context.h>

#include <cassert>
#include <cerrno>
#include <mutex>

namespace bupp {

using detail::native_worker;

namespace {

[[nodiscard]] std::error_code make_submit_error(int result) noexcept {
  return std::error_code(-result, std::generic_category());
}

[[nodiscard]] std::size_t count_operations(
    io_context::operation_base* operations) noexcept {
  std::size_t count = 0;
  while (operations != nullptr) {
    ++count;
    operations = operations->pending_next;
  }
  return count;
}

void push_operation(std::atomic<io_context::operation_base*>& stack,
                    io_context::operation_base& operation) noexcept {
  io_context::operation_base* head = stack.load(std::memory_order_acquire);
  do {
    operation.pending_next = head;
  } while (!stack.compare_exchange_weak(
      head, &operation, std::memory_order_release, std::memory_order_acquire));
}

void push_operation_chain(std::atomic<io_context::operation_base*>& stack,
                          io_context::operation_base* operations) noexcept {
  if (operations == nullptr) {
    return;
  }

  io_context::operation_base* tail = operations;
  while (tail->pending_next != nullptr) {
    tail = tail->pending_next;
  }

  io_context::operation_base* head = stack.load(std::memory_order_acquire);
  do {
    tail->pending_next = head;
  } while (!stack.compare_exchange_weak(
      head, operations, std::memory_order_release, std::memory_order_acquire));
}

}  // namespace

std::size_t io_context::queued_io_size() const noexcept {
  return queued_io_.pending_count.load(std::memory_order_acquire);
}

void io_context::enqueue_io(operation_base& operation) noexcept {
  operation.native_worker_ = nullptr;

  const std::size_t prev =
      queued_io_.pending_count.fetch_add(1, std::memory_order_acq_rel);

  push_operation(queued_io_.global_pending_head, operation);

  const bool reached_max = native_.options.max_queued_io_operations > 0 &&
                           prev + 1 >= native_.options.max_queued_io_operations;

  if (native_.options.queued_io_flush_after <= duration::zero()) {
    (void)flush_io_queue(select_io_worker(), true);
    return;
  }

  if (prev == 0) {
    arm_flush_timer();
  }
  if (reached_max) {
    (void)flush_io_queue(select_io_worker(), false);
  }
}

void io_context::submit_direct(operation_base& operation) noexcept {
  native_worker& worker = ensure_operation_worker(operation);
  async_io::linux_native::io_uring_context& native_context =
      *worker.context.load(std::memory_order_acquire);
  const int result = native_context.submit(operation);
  if (result < 0) {
    operation.complete_submit_error(result);
    (void)native_context.post(operation);
  }
}

void io_context::post(
    async_io::linux_native::io_uring_operation_base& operation) noexcept {
  native_worker& worker = select_worker();
  async_io::linux_native::io_uring_context* native_context =
      worker.context.load(std::memory_order_acquire);
  if (native_context != nullptr) {
    (void)native_context->post(operation);
  }
}

std::error_code io_context::flush_io_queue() noexcept {
  return flush_io_queue(false);
}

std::error_code io_context::flush_io_queue(bool wait_for_gate) noexcept {
  std::error_code first_error;
  const std::size_t worker_count = std::max<std::size_t>(
      1, native_workers_.active_count.load(std::memory_order_acquire));

  native_worker* worker = native_workers_.head;
  for (std::size_t index = 0; index < worker_count && worker != nullptr;
       ++index) {
    if (worker->context.load(std::memory_order_acquire) != nullptr) {
      const std::error_code error = flush_io_queue(*worker, wait_for_gate);
      if (!first_error && error) {
        first_error = error;
      }
    }
    worker = worker->next.load(std::memory_order_acquire);
  }
  return first_error;
}

std::error_code io_context::flush_io_queue(native_worker& worker,
                                           bool wait_for_gate) noexcept {
  async_io::linux_native::io_uring_context* worker_context =
      worker.context.load(std::memory_order_acquire);
  if (worker_context == nullptr) {
    return {};
  }

  async_io::linux_native::io_uring_context& native_context = *worker_context;
  auto lock = wait_for_gate ? native_context.lock_uring()
                            : native_context.try_lock_uring();
  if (!lock) {
    if (queued_io_size() != 0 &&
        native_.options.queued_io_flush_after > duration::zero()) {
      arm_flush_timer();
    }
    return {};
  }

  operation_base* operations = take_pending_io(worker);
  const std::size_t operation_count = count_operations(operations);
  if (operations == nullptr) {
    lock.reset();
    if (queued_io_size() != 0 &&
        native_.options.queued_io_flush_after > duration::zero()) {
      arm_flush_timer();
    }
    return {};
  }

  std::error_code error = flush_operations(operations, native_context, lock);
  [[maybe_unused]] const std::size_t prev = queued_io_.pending_count.fetch_sub(
      operation_count, std::memory_order_acq_rel);
  assert(prev >= operation_count);

  if (native_.options.queued_io_flush_after > duration::zero()) {
    (void)queued_io_flush_timer_.cancel();
  }

  if (queued_io_size() != 0 &&
      native_.options.queued_io_flush_after > duration::zero()) {
    arm_flush_timer();
  }

  return error;
}

// Fill SQEs from *chain into the io_uring ring until the ring is full or the
// chain is exhausted.  Operations that fail preparation (non-EAGAIN errors)
// are detached from the chain and appended to **failed_tail via a
// double-pointer.
//
// Returns the first operation that could not be prepared (-EAGAIN), or nullptr
// if the entire chain was consumed.
static io_context::operation_base* try_prepare_batch(
    io_context::operation_base* chain,
    async_io::linux_native::io_uring_context& native_ctx,
    io_context::operation_base**& failed_tail, int& first_error) noexcept {
  while (chain != nullptr) {
    io_context::operation_base* next = chain->pending_next;
    chain->pending_next = nullptr;

    const int result = native_ctx.prepare_locked(*chain);

    if (result == -EAGAIN) {
      chain->pending_next = next;  // restore link, caller will retry
      return chain;
    }

    if (result < 0) {
      chain->complete_submit_error(result);
      if (first_error == 0) first_error = result;
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

std::error_code io_context::flush_operations(
    operation_base* operations,
    async_io::linux_native::io_uring_context& native_context,
    async_io::linux_native::io_uring_context::uring_lock& lock) noexcept {
  if (operations == nullptr) {
    lock.reset();
    return {};
  }

  int first_error = 0;
  operation_base* failed_head = nullptr;
  operation_base** failed_tail = &failed_head;

  while (operations != nullptr) {
    operations =
        try_prepare_batch(operations, native_context, failed_tail, first_error);
    const int result = native_context.submit_locked();
    if (result < 0 && first_error == 0) first_error = result;
  }

  lock.reset();

  while (failed_head != nullptr) {
    operation_base* op = failed_head;
    failed_head = failed_head->pending_next;
    op->pending_next = nullptr;
    (void)native_context.post(*op);
  }

  if (first_error < 0) return make_submit_error(first_error);
  return {};
}

io_context::operation_base* io_context::take_pending_io(
    native_worker& worker) noexcept {
  move_global_io_to_worker(worker);
  return take_worker_pending_io(worker);
}

void io_context::move_global_io_to_worker(native_worker& worker) noexcept {
  operation_base* operations = queued_io_.global_pending_head.exchange(
      nullptr, std::memory_order_acq_rel);
  if (operations == nullptr) {
    return;
  }

  operation_base* current = operations;
  while (current != nullptr) {
    current->native_worker_ = &worker;
    current = current->pending_next;
  }
  push_operation_chain(worker.pending_io_head, operations);
}

io_context::operation_base* io_context::take_worker_pending_io(
    native_worker& worker) noexcept {
  return worker.pending_io_head.exchange(nullptr, std::memory_order_acq_rel);
}

void io_context::arm_flush_timer() noexcept {
  if (native_.options.queued_io_flush_after <= duration::zero()) {
    return;
  }

  const time_point deadline =
      clock::now() + native_.options.queued_io_flush_after;
  bool should_post_driver = false;
  {
    std::lock_guard context_lock(timers_.mutex);
    detail::timer_slot& timer = queued_io_flush_timer_.timer_;
    std::lock_guard timer_lock(timer.mutex);
    if (timer.context != this || !timers_.queued_io_flush_wait.has_value() ||
        !timers_.queue_flush_wait()) {
      return;
    }

    timer.expiry = deadline;
    ++timer.generation;
    push_timer_operation(timer.submitted_head, *timers_.queued_io_flush_wait);
    if (timers_.queue_driver()) {
      should_post_driver = true;
    }
  }

  if (should_post_driver) {
    (void)primary_native_context().post(timer_driver_operation_);
  }
}

}  // namespace bupp
