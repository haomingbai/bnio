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

bool io_uring_context::collect_ready_cqes() noexcept {
  operation_queue cqe_tasks;
  const unsigned task_count = collect_cqe_tasks(cqe_tasks);
  if (task_count == 0) {
    return false;
  }

  dispatch_cqe_tasks(cqe_tasks, task_count);
  return true;
}

unsigned io_uring_context::collect_cqe_tasks(
    operation_queue& cqe_tasks) noexcept {
  if (!ring_.is_open()) {
    return 0;
  }

  unsigned task_count = 0;
  (void)ring_.consume_ready_cqes(
      options_.cqe_batch_window,
      [this, &cqe_tasks,
       &task_count](bupp::base::completion_queue_entry cqe) noexcept {
        cqe_data data;
        data.user_data = cqe.get_data();
        data.result = cqe.res();
        data.flags = cqe.flags();

        if (data.user_data == eventfd_user_data()) {
          eventfd_poll_pending_ = false;
          drain_eventfd();
          (void)submit_eventfd_poll();
          return;
        }

        if (enqueue_cqe_task(data, cqe_tasks)) {
          ++task_count;
        }
      });
  return task_count;
}

void io_uring_context::dispatch_cqe_tasks(
    operation_queue& cqe_tasks, unsigned task_count) noexcept {
  // Tier 1 — inline: small batch, always push to the local queue.
  if (task_count <= options_.cqe_inline_completion_threshold) {
    local_tasks_.push(reverse_tasks(cqe_tasks.pop_all()));
    return;
  }

  // Tier 2 — local queue: within the per-iteration budget.
  // When local_queue_threshold is 0 (default) this tier is unlimited and
  // CQEs never spill to the shared CPU queue on this path.
  if (options_.local_queue_threshold == 0 ||
      (local_task_budget_ > 0 && task_count <= local_task_budget_)) {
    local_tasks_.push(reverse_tasks(cqe_tasks.pop_all()));
    if (options_.local_queue_threshold > 0) {
      local_task_budget_ -= task_count;
    }
    return;
  }

  // Tier 3: local budget exhausted or batch exceeds remaining budget; publish
  // to the shared CPU queue for a later run-loop pass.
  push_cpu_tasks(cqe_tasks);
}

bool io_uring_context::enqueue_cqe_task(const cqe_data& data,
                                        operation_queue& tasks) noexcept {
  if (data.user_data == eventfd_user_data()) {
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
