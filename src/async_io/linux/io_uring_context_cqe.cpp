/**
 * @file io_uring_context_cqe.cpp
 * @brief CQE collection and tiered dispatch (inline, local, shared CPU queue).
 */

#include <bnio/base/linux/completion_queue_entry.h>

#include <cerrno>

#include "io_uring_context_internal.h"

namespace bnio::async_io::linux_native {

int io_uring_context::wait_for_cqe_event(__kernel_timespec* timeout) noexcept {
  if (!ring_.is_open()) {
    return -EINVAL;
  }

  if (timeout != nullptr) {
    bnio::base::completion_queue_entry cqe;
    return ring_.wait_cqe_timeout(cqe, timeout);
  }

  // Do not retry on -EINTR here: propagate it so the run loop can
  // re-evaluate should_finish() (e.g. a signal-driven stop()).
  return bnio::base::ring::wait_cqe_event(ring_.native_fd(), 1);
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

io_uring_context::cqe_user_data_kind io_uring_context::classify_cqe_user_data(
    void* user_data) noexcept {
  if (user_data == eventfd_user_data()) {
    return cqe_user_data_kind::eventfd;
  }
  if (user_data == local_eventfd_user_data()) {
    return cqe_user_data_kind::local_eventfd;
  }
  return cqe_user_data_kind::operation;
}

struct io_uring_context::cqe_collector {
  io_uring_context& context;
  operation_queue& tasks;
  unsigned count = 0;

  void operator()(bnio::base::completion_queue_entry cqe) noexcept {
    cqe_data data;
    data.user_data = cqe.get_data();
    data.result = cqe.res();
    data.flags = cqe.flags();

    switch (context.classify_cqe_user_data(data.user_data)) {
      case cqe_user_data_kind::eventfd:
        // Self-pipe notification: the eventfd poll CQE arrived.
        // Drain all queued eventfd notifications and re-arm the poll
        // so the ring can wake us again when new work arrives.
        context.poll_state_.eventfd_poll_pending = false;
        context.drain_eventfd();
        // Re-arm failures are deliberately not terminal here: a
        // transient -EAGAIN under SQ pressure must not half-close the
        // context, and a fatal failure (e.g. a closed wake channel) is
        // handled by the single re-arm policy point in
        // wait_for_io_work(), which retries transient failures and
        // routes fatal ones through the finish drain. Storing
        // finishing here used to leave the poll unarmed while the
        // state check above kept reporting "stopping", letting the
        // worker park in io_uring_enter with no wake source.
        (void)context.submit_eventfd_poll();
        return;

      case cqe_user_data_kind::local_eventfd:
        // Directed wake: only this worker is signalled. Drain the
        // per-worker channel and re-arm its poll. Re-arm failures
        // follow the same policy as the shared channel above.
        context.poll_state_.local_eventfd_poll_pending = false;
        (void)context.local_state_.wake_channel_.drain();
        (void)context.submit_local_eventfd_poll();
        return;

      case cqe_user_data_kind::operation:
        if (context.enqueue_cqe_task(data, tasks)) {
          ++count;
        }
        return;
    }
  }
};

unsigned io_uring_context::collect_cqe_tasks(
    operation_queue& cqe_tasks) noexcept {
  if (!ring_.is_open()) {
    return 0;
  }

  cqe_collector collector{*this, cqe_tasks};
  (void)ring_.consume_ready_cqes(options_.cqe_batch_window, collector);
  return collector.count;
}

void io_uring_context::dispatch_cqe_tasks(operation_queue& cqe_tasks,
                                          unsigned task_count) noexcept {
  // Tier 1 — inline: small batch, always push to the local queue.
  if (task_count <= options_.cqe_inline_completion_threshold) {
    local_state_.push_cpu(cqe_tasks.pop_all());
    return;
  }

  // Tier 2 — local queue: within the per-iteration budget.
  // When local_queue_threshold is 0 (default) this tier is unlimited and
  // CQEs never spill to the shared CPU queue on this path.
  if (options_.local_queue_threshold == 0 ||
      (scheduling_state_.local_task_budget > 0 &&
       task_count <= scheduling_state_.local_task_budget)) {
    local_state_.push_cpu(cqe_tasks.pop_all());
    if (options_.local_queue_threshold > 0) {
      scheduling_state_.local_task_budget -= task_count;
    }
    return;
  }

  // Tier 3: local budget exhausted or batch exceeds remaining budget; publish
  // to the shared CPU queue for a later run-loop pass.
  push_cpu_tasks(cqe_tasks);
}

bool io_uring_context::enqueue_cqe_task(const cqe_data& data,
                                        operation_queue& tasks) noexcept {
  auto* operation = static_cast<io_uring_operation_base*>(data.user_data);
  if (operation == nullptr) {
    return false;
  }

  // Cast to I/O base; all CQEs (other than eventfd) originate from I/O ops.
  auto* io_op = static_cast<io_uring_io_operation_base*>(operation);
  remove_inflight(*io_op);

  operation->result = data.result;
  operation->flags = data.flags;
  tasks.push(*operation);
  return true;
}

}  // namespace bnio::async_io::linux_native
