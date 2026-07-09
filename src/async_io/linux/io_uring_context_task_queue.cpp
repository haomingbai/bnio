#include <cerrno>

#include "io_uring_context_internal.h"

namespace bupp::async_io::linux_native {

int io_uring_context::post(io_uring_operation_base& operation) noexcept {
  assert_running();

  if (current_context_ == this && current_local_tasks_ != nullptr) {
    current_local_tasks_->push(operation);
    return 0;
  }

  push_global_task(operation);

  if (io_waiter_active_.load(std::memory_order_acquire)) {
    return submit_wake_task();
  }
  return 0;
}

void io_uring_context::push_global_task(
    io_uring_operation_base& operation) noexcept {
  io_uring_operation_base* current_head =
      global_tasks_.load(std::memory_order_acquire);
  do {
    operation.next = current_head;
  } while (!global_tasks_.compare_exchange_weak(current_head, &operation,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire));
  notify_waiters();
}

void io_uring_context::push_global_tasks(operation_queue& operations) noexcept {
  io_uring_operation_base* ordered_tasks = reverse_tasks(operations.pop_all());
  if (ordered_tasks == nullptr) {
    return;
  }

  while (ordered_tasks != nullptr) {
    io_uring_operation_base* operation = ordered_tasks;
    ordered_tasks = ordered_tasks->next;

    io_uring_operation_base* current_head =
        global_tasks_.load(std::memory_order_acquire);
    do {
      operation->next = current_head;
    } while (!global_tasks_.compare_exchange_weak(current_head, operation,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire));
  }
  notify_waiters();
}

bool io_uring_context::move_global_tasks(
    operation_queue& local_tasks) noexcept {
  io_uring_operation_base* incoming =
      global_tasks_.exchange(nullptr, std::memory_order_acq_rel);
  if (incoming == nullptr) {
    return false;
  }

  local_tasks.push(reverse_tasks(incoming));
  return true;
}

void io_uring_context::notify_waiters() noexcept {
  std::lock_guard lock(wait_mutex_);
  wait_cv_.notify_all();
}

int io_uring_context::submit_wake_task() noexcept {
  auto lock = lock_uring();
  return submit_wake_task_locked();
}

int io_uring_context::submit_wake_task_locked() noexcept {
  if (!ring_.is_open()) {
    return -EINVAL;
  }
  if (wake_task_pending_) {
    return 0;
  }

  bupp::base::submission_queue_entry sqe = ring_.get_sqe();
  if (sqe.raw() == nullptr) {
    return -EAGAIN;
  }

  sqe.prep_nop();
  sqe.set_data(wake_user_data());

  const int submit_result = ring_.submit();
  if (submit_result <= 0) {
    return submit_result < 0 ? submit_result : -EAGAIN;
  }

  wake_task_pending_ = true;
  return submit_result;
}

void* io_uring_context::wake_user_data() noexcept {
  static int wake_sentinel = 0;
  return &wake_sentinel;
}

}  // namespace bupp::async_io::linux_native
