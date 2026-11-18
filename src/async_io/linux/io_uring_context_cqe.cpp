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

  // Propagate -EINTR so the run loop can re-evaluate should_finish()
  // (e.g. a signal-driven stop()).
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
        // Self-pipe: drain and re-arm; failures are ignored here because
        // wait_for_io_work() owns the retry/fatal policy.
        context.poll_state_.eventfd_poll_pending = false;
        context.drain_eventfd();
        (void)context.arm_wake_poll(
            context.global_state_->wake_channel_.read_fd(), eventfd_user_data(),
            context.poll_state_.eventfd_poll_pending);
        return;

      case cqe_user_data_kind::local_eventfd:
        // Directed wake: drain and re-arm this worker's channel; failures
        // follow the shared-channel policy above.
        context.poll_state_.local_eventfd_poll_pending = false;
        (void)context.local_state_.wake_channel_.drain();
        (void)context.arm_wake_poll(
            context.local_state_.wake_channel_.read_fd(),
            local_eventfd_user_data(),
            context.poll_state_.local_eventfd_poll_pending);
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
  // Tier 1: small batch, always local.
  if (task_count <= options_.cqe_inline_completion_threshold) {
    local_state_.push_cpu(cqe_tasks.pop_all());
    return;
  }

  // Tier 2: within the per-iteration budget (threshold 0 = unlimited).
  if (options_.local_queue_threshold == 0 ||
      (scheduling_state_.local_task_budget > 0 &&
       task_count <= scheduling_state_.local_task_budget)) {
    local_state_.push_cpu(cqe_tasks.pop_all());
    if (options_.local_queue_threshold > 0) {
      scheduling_state_.local_task_budget -= task_count;
    }
    return;
  }

  // Tier 3: budget exhausted or batch too large; publish to the shared queue.
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

  // -EAGAIN reaches here only for REQ_F_NOWAIT requests (sockets become an
  // armed poll internally). Re-enter the submission pipeline instead of
  // delivering a terminal error; the operation stays in the pending state.
  if (data.result == -EAGAIN && io_op->rearm_on_eagain()) {
    local_state_.push_io(*io_op);
    return false;
  }

  tasks.push(*operation);
  return true;
}

}  // namespace bnio::async_io::linux_native
