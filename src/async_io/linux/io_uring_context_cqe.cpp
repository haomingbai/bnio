#include <bupp/base/linux/completion_queue_entry.h>

#include <cerrno>

#include "io_uring_context_internal.h"

namespace bupp::async_io::linux_native {

int io_uring_context::wait_for_cqe_event() noexcept {
  int ring_fd = -1;
  if (!ring_.is_open()) {
    return -EINVAL;
  }
  ring_fd = ring_.native_fd();

  for (;;) {
    const int result = bupp::base::ring::wait_cqe_event(ring_fd, 1);
    if (result == -EINTR) {
      continue;
    }
    return result;
  }
}

bool io_uring_context::collect_ready_cqes(operation_queue& local_tasks,
                                          unsigned& local_task_budget,
                                          bool wait_for_gate) noexcept {
  operation_queue cqe_tasks;
  const unsigned task_count = collect_cqe_tasks(cqe_tasks, wait_for_gate);
  if (task_count == 0) {
    return false;
  }

  dispatch_cqe_tasks(cqe_tasks, task_count, local_tasks, local_task_budget);
  return true;
}

unsigned io_uring_context::collect_cqe_tasks(operation_queue& cqe_tasks,
                                             bool wait_for_gate) noexcept {
  auto lock = wait_for_gate ? lock_uring() : try_lock_uring();
  if (!lock) {
    return 0;
  }

  if (!ring_.is_open()) {
    return 0;
  }

  unsigned task_count = 0;
  (void)ring_.consume_ready_cqes(
      cqe_batch_window_, [this, &cqe_tasks, &task_count](
                             bupp::base::completion_queue_entry cqe) noexcept {
        cqe_data data;
        data.user_data = cqe.get_data();
        data.result = cqe.res();
        data.flags = cqe.flags();

        if (data.user_data == wake_user_data()) {
          wake_task_pending_ = false;
        }

        if (enqueue_cqe_task(data, cqe_tasks)) {
          ++task_count;
        }
      });
  return task_count;
}

void io_uring_context::dispatch_cqe_tasks(
    operation_queue& cqe_tasks, unsigned task_count,
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept {
  // Tier 1 — inline: small batch, always push to the local queue.
  if (task_count <= cqe_inline_completion_threshold_) {
    local_tasks.push(reverse_tasks(cqe_tasks.pop_all()));
    return;
  }

  // Tier 2 — local queue: within the per-iteration budget.
  // When local_queue_threshold_ is 0 (default) this tier is unlimited and
  // CQEs never spill to the global queue on this path.
  if (local_queue_threshold_ == 0 ||
      (local_task_budget > 0 && task_count <= local_task_budget)) {
    local_tasks.push(reverse_tasks(cqe_tasks.pop_all()));
    if (local_queue_threshold_ > 0) {
      local_task_budget -= task_count;
    }
    return;
  }

  // Tier 3 — global: local budget exhausted or batch exceeds remaining
  // budget; publish to the global (cross-thread) queue.
  push_global_tasks(cqe_tasks);
}

bool io_uring_context::enqueue_cqe_task(const cqe_data& data,
                                        operation_queue& tasks) noexcept {
  if (data.user_data == wake_user_data()) {
    return false;
  }

  auto* operation = static_cast<io_uring_operation_base*>(data.user_data);
  if (operation == nullptr) {
    return false;
  }

  operation->result = data.result;
  operation->flags = data.flags;
  tasks.push(*operation);
  return true;
}

}  // namespace bupp::async_io::linux_native
