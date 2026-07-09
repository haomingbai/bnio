#include <bupp/linux/io_context.h>

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

}  // namespace

std::size_t io_context::queued_io_size() const noexcept {
  return pending_io_count_.load(std::memory_order_acquire);
}

void io_context::enqueue_io(operation_base& operation) noexcept {
  // Lock-free push onto the pending-I/O stack (CAS, same pattern as
  // push_timer_operation).  No allocation, no mutex.
  operation_base* current_head =
      pending_io_head_.load(std::memory_order_acquire);
  do {
    operation.pending_next = current_head;
  } while (!pending_io_head_.compare_exchange_weak(current_head, &operation,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire));

  const std::size_t prev =
      pending_io_count_.fetch_add(1, std::memory_order_acq_rel);

  if (prev == 0) {
    arm_flush_timer();
  }
  if (linux_options_.max_queued_io_operations > 0 &&
      prev + 1 >= linux_options_.max_queued_io_operations) {
    (void)flush_io_queue();
  }
}

void io_context::submit_direct(operation_base& operation) noexcept {
  const int result = native_context_.submit(operation);
  if (result < 0) {
    operation.complete_submit_error(result);
    (void)native_context_.post(operation);
  }
}

void io_context::post(
    async_io::linux_native::io_uring_operation_base& operation) noexcept {
  (void)native_context_.post(operation);
}

std::error_code io_context::flush_io_queue() noexcept {
  operation_base* operations = take_pending_io();
  if (operations == nullptr) {
    return {};
  }
  if (linux_options_.queued_io_flush_after > duration::zero()) {
    (void)queued_io_flush_timer_.cancel();
  }
  return flush_operations(operations);
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
    operation_base* operations) noexcept {
  if (operations == nullptr) return {};

  int first_error = 0;
  operation_base* failed_head = nullptr;
  operation_base** failed_tail = &failed_head;

  {
    auto lock = native_context_.lock_uring();

    while (operations != nullptr) {
      operations = try_prepare_batch(operations, native_context_, failed_tail,
                                     first_error);
      const int result = native_context_.submit_locked();
      if (result < 0 && first_error == 0) first_error = result;
    }
  }

  native_context_.notify_waiters();

  while (failed_head != nullptr) {
    operation_base* op = failed_head;
    failed_head = failed_head->pending_next;
    op->pending_next = nullptr;
    (void)native_context_.post(*op);
  }

  if (first_error < 0) return make_submit_error(first_error);
  return {};
}

io_context::operation_base* io_context::take_pending_io() noexcept {
  operation_base* operations =
      pending_io_head_.exchange(nullptr, std::memory_order_acq_rel);
  pending_io_count_.store(0, std::memory_order_release);
  if (operations != nullptr) {
    operations = reverse_operations(operations);
  }
  return operations;
}

void io_context::arm_flush_timer() noexcept {
  if (linux_options_.queued_io_flush_after <= duration::zero()) {
    (void)flush_io_queue();
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
    (void)native_context_.post(timer_driver_operation_);
  }
}

}  // namespace bupp
