#include "io_uring_context_internal.h"

namespace bupp::async_io::linux_native {

void io_uring_context::run() noexcept {
  assert_running();
  bool expected_active = false;
  if (!run_active_.compare_exchange_strong(expected_active, true,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
    return;
  }

  if (!is_open()) {
    run_active_.store(false, std::memory_order_release);
    return;
  }

  operation_queue local_tasks;
  io_uring_context* previous_context = current_context_;
  current_context_ = this;
  this->local_tasks_ = &local_tasks;
  waiting_.store(false, std::memory_order_release);
  options_.task_queue->awake_workers.fetch_add(1, std::memory_order_acq_rel);

  unsigned local_task_budget = 0;
  run_phase phase = run_phase::run_ready_tasks;

  while (phase != run_phase::finished) {
    switch (phase) {
      case run_phase::run_ready_tasks:
        phase = handle_run_ready_tasks(local_tasks, local_task_budget);
        break;

      case run_phase::wait_for_work:
        phase = handle_wait_for_work(local_tasks, local_task_budget);
        break;

      case run_phase::finish_drain:
        phase = handle_finish_drain(local_tasks, local_task_budget);
        break;

      case run_phase::finished:
        break;
    }
  }

  this->local_tasks_ = nullptr;
  current_context_ = previous_context;
  options_.task_queue->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
  run_active_.store(false, std::memory_order_release);
}

int io_uring_context::stop() noexcept {
  context_state expected = context_state::running;
  if (!state_.compare_exchange_strong(expected, context_state::finishing,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire) &&
      expected != context_state::finishing) {
    return 0;
  }

  return signal_eventfd();
}

bool io_uring_context::is_in_context() const noexcept {
  return current_context_ == this;
}

io_uring_context::run_phase io_uring_context::handle_run_ready_tasks(
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept {
  // Reset the local-queue budget for this pass through the run loop.
  local_task_budget = options_.local_queue_threshold;

  if (io_uring_operation_base* operations =
          reverse_tasks(local_tasks.pop_all())) {
    execute_tasks(operations);
    return run_phase::run_ready_tasks;
  }

  if (move_cpu_tasks(local_tasks)) {
    return run_phase::run_ready_tasks;
  }

  if (run_pending_work()) {
    return run_phase::run_ready_tasks;
  }

  return should_finish() ? run_phase::finish_drain : run_phase::wait_for_work;
}

io_uring_context::run_phase io_uring_context::handle_wait_for_work(
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept {
  const run_phase spin_result = spin_for_work(local_tasks, local_task_budget);
  if (spin_result != run_phase::wait_for_work) {
    return spin_result;
  }

  return wait_for_io_work(local_tasks, local_task_budget);
}

io_uring_context::run_phase io_uring_context::handle_finish_drain(
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept {
  finish(local_tasks, local_task_budget);
  return run_phase::finished;
}

io_uring_context::run_phase io_uring_context::spin_for_work(
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept {
  for (unsigned round = 0; round < options_.wait_spin_count; ++round) {
    if (collect_ready_cqes(local_tasks, local_task_budget) ||
        move_cpu_tasks(local_tasks)) {
      return run_phase::run_ready_tasks;
    }
    if (should_finish()) {
      return run_phase::finish_drain;
    }
  }

  return run_phase::wait_for_work;
}

io_uring_context::run_phase io_uring_context::wait_for_io_work(
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept {
  begin_wait();

  if (collect_ready_cqes(local_tasks, local_task_budget) ||
      move_cpu_tasks(local_tasks) || run_pending_work() || should_finish()) {
    end_wait();
    return should_finish() ? run_phase::finish_drain
                           : run_phase::run_ready_tasks;
  }

  (void)submit_eventfd_poll();
  const int wait_result = wait_for_cqe_event();
  end_wait();

  if (wait_result < 0 && !should_finish()) {
    return run_phase::finished;
  }

  if (collect_ready_cqes(local_tasks, local_task_budget) ||
      move_cpu_tasks(local_tasks)) {
    return run_phase::run_ready_tasks;
  }

  return should_finish() ? run_phase::finish_drain : run_phase::wait_for_work;
}

bool io_uring_context::should_finish() const noexcept {
  return state_.load(std::memory_order_acquire) != context_state::running;
}

void io_uring_context::finish(operation_queue& local_tasks,
                              unsigned& local_task_budget) noexcept {
  for (;;) {
    (void)move_cpu_tasks(local_tasks);
    (void)collect_ready_cqes(local_tasks, local_task_budget);
    (void)move_cpu_tasks(local_tasks);

    io_uring_operation_base* operations = reverse_tasks(local_tasks.pop_all());
    if (operations == nullptr) {
      break;
    }
    execute_tasks(operations);
  }

  state_.store(context_state::finished, std::memory_order_release);
}

}  // namespace bupp::async_io::linux_native
