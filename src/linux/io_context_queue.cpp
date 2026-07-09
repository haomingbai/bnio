#include <bupp/linux/io_context.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <mutex>

namespace bupp {

namespace {

[[nodiscard]] io_context::operation_base* reverse_operations(
    io_context::operation_base* operations) noexcept {
  io_context::operation_base* reversed = nullptr;
  while (operations != nullptr) {
    io_context::operation_base* operation = operations;
    operations = operations->pending_next;
    operation->pending_next = reversed;
    reversed = operation;
  }
  return reversed;
}

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

}  // namespace

std::size_t io_context::queued_io_size() const noexcept {
  return pending_io_count_.load(std::memory_order_acquire);
}

void io_context::enqueue_io(operation_base& operation) noexcept {
  native_worker& worker = ensure_operation_worker(operation);
  const std::size_t prev =
      pending_io_count_.fetch_add(1, std::memory_order_acq_rel);
  const std::size_t worker_prev =
      worker.pending_io_count.fetch_add(1, std::memory_order_acq_rel);

  operation_base* current_head =
      worker.pending_io_head.load(std::memory_order_acquire);
  do {
    operation.pending_next = current_head;
  } while (!worker.pending_io_head.compare_exchange_weak(
      current_head, &operation, std::memory_order_acq_rel,
      std::memory_order_acquire));

  const std::size_t active_workers = std::max<std::size_t>(
      1, active_native_worker_count_.load(std::memory_order_acquire));
  const std::size_t worker_flush_threshold =
      linux_options_.max_queued_io_operations == 0
          ? 0
          : std::max<std::size_t>(
                1, linux_options_.max_queued_io_operations / active_workers);
  const bool reached_max =
      worker_flush_threshold > 0 && worker_prev + 1 >= worker_flush_threshold;
  if (linux_options_.queued_io_flush_after <= duration::zero()) {
    if (worker_prev == 0 || reached_max) {
      (void)flush_io_queue(worker, true);
    }
    return;
  }

  if (prev == 0) {
    arm_flush_timer();
  }
  if (reached_max) {
    (void)flush_io_queue(worker, false);
  }
}

void io_context::submit_direct(operation_base& operation) noexcept {
  native_worker& worker = ensure_operation_worker(operation);
  async_io::linux_native::io_uring_context& native_context = *worker.context;
  const int result = native_context.submit(operation);
  if (result < 0) {
    operation.complete_submit_error(result);
    (void)native_context.post(operation);
  }
}

void io_context::post(
    async_io::linux_native::io_uring_operation_base& operation) noexcept {
  native_worker& worker = select_worker();
  (void)worker.context->post(operation);
}

std::error_code io_context::flush_io_queue() noexcept {
  return flush_io_queue(false);
}

std::error_code io_context::flush_io_queue(bool wait_for_gate) noexcept {
  std::error_code first_error;
  const std::size_t worker_count =
      active_native_worker_count_.load(std::memory_order_acquire);
  for (std::size_t index = 0; index < worker_count; ++index) {
    native_worker* worker = native_workers_[index].get();
    if (worker == nullptr || worker->context == nullptr) {
      continue;
    }
    const std::error_code error = flush_io_queue(*worker, wait_for_gate);
    if (!first_error && error) {
      first_error = error;
    }
  }
  return first_error;
}

std::error_code io_context::flush_io_queue(native_worker& worker,
                                           bool wait_for_gate) noexcept {
  async_io::linux_native::io_uring_context& native_context = *worker.context;
  auto lock = wait_for_gate ? native_context.lock_uring()
                            : native_context.try_lock_uring();
  if (!lock) {
    if (queued_io_size() != 0 &&
        linux_options_.queued_io_flush_after > duration::zero()) {
      arm_flush_timer();
    }
    return {};
  }

  operation_base* operations = take_pending_io(worker);
  const std::size_t operation_count = count_operations(operations);
  if (operations == nullptr) {
    lock.reset();
    if (queued_io_size() != 0 &&
        linux_options_.queued_io_flush_after > duration::zero()) {
      arm_flush_timer();
    }
    return {};
  }

  std::error_code error = flush_operations(operations, native_context, lock);
  [[maybe_unused]] const std::size_t prev =
      pending_io_count_.fetch_sub(operation_count, std::memory_order_acq_rel);
  assert(prev >= operation_count);
  [[maybe_unused]] const std::size_t worker_prev =
      worker.pending_io_count.fetch_sub(operation_count,
                                        std::memory_order_acq_rel);
  assert(worker_prev >= operation_count);

  if (linux_options_.queued_io_flush_after > duration::zero()) {
    (void)queued_io_flush_timer_.cancel();
  }

  if (queued_io_size() != 0 &&
      linux_options_.queued_io_flush_after > duration::zero()) {
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
    io_context::operation_base** failed_tail, int& first_error) noexcept {
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
  operation_base* operations =
      worker.pending_io_head.exchange(nullptr, std::memory_order_acq_rel);
  if (operations != nullptr) {
    operations = reverse_operations(operations);
  }
  return operations;
}

void io_context::arm_flush_timer() noexcept {
  if (linux_options_.queued_io_flush_after <= duration::zero()) {
    return;
  }

  const time_point deadline =
      clock::now() + linux_options_.queued_io_flush_after;
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
