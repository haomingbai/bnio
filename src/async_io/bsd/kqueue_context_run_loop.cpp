#include "kqueue_context_internal.h"

namespace bupp::async_io::bsd_native {

void kqueue_context::run() noexcept {
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
  kqueue_context* previous_context = current_context_;
  current_context_ = this;
  local_tasks_ = &local_tasks;

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

  local_tasks_ = nullptr;
  current_context_ = previous_context;
  run_active_.store(false, std::memory_order_release);
}

int kqueue_context::stop() noexcept {
  context_state expected = context_state::running;
  if (!state_.compare_exchange_strong(expected, context_state::finishing,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire) &&
      expected != context_state::finishing) {
    return 0;
  }
  return trigger_wakeup();
}

bool kqueue_context::is_in_context() const noexcept {
  return current_context_ == this;
}

kqueue_context::run_phase kqueue_context::handle_run_ready_tasks(
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept {
  local_task_budget = local_queue_threshold_;

  if (kqueue_operation_base* operations =
          reverse_tasks(local_tasks.pop_all())) {
    execute_tasks(operations);
    return run_phase::run_ready_tasks;
  }
  if (move_posted_tasks(local_tasks)) {
    return run_phase::run_ready_tasks;
  }
  return should_finish() ? run_phase::finish_drain : run_phase::wait_for_work;
}

kqueue_context::run_phase kqueue_context::handle_wait_for_work(
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept {
  const run_phase spin_result = spin_for_work(local_tasks, local_task_budget);
  if (spin_result != run_phase::wait_for_work) {
    return spin_result;
  }
  return wait_for_io_work(local_tasks, local_task_budget);
}

kqueue_context::run_phase kqueue_context::handle_finish_drain(
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept {
  finish(local_tasks, local_task_budget);
  return run_phase::finished;
}

kqueue_context::run_phase kqueue_context::spin_for_work(
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept {
  for (unsigned round = 0; round < wait_spin_count_; ++round) {
    if (collect_ready_events(local_tasks, local_task_budget, false) ||
        move_posted_tasks(local_tasks)) {
      return run_phase::run_ready_tasks;
    }
    if (should_finish()) {
      return run_phase::finish_drain;
    }
  }
  return run_phase::wait_for_work;
}

kqueue_context::run_phase kqueue_context::wait_for_io_work(
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept {
  if (collect_ready_events(local_tasks, local_task_budget, false) ||
      move_posted_tasks(local_tasks) || should_finish()) {
    return should_finish() ? run_phase::finish_drain
                           : run_phase::run_ready_tasks;
  }

  (void)collect_ready_events(local_tasks, local_task_budget, true);
  if (!local_tasks.empty() || move_posted_tasks(local_tasks)) {
    return run_phase::run_ready_tasks;
  }
  return should_finish() ? run_phase::finish_drain : run_phase::wait_for_work;
}

bool kqueue_context::should_finish() const noexcept {
  return state_.load(std::memory_order_acquire) != context_state::running;
}

void kqueue_context::finish(operation_queue& local_tasks,
                            unsigned& local_task_budget) noexcept {
  for (;;) {
    (void)move_posted_tasks(local_tasks);
    (void)collect_ready_events(local_tasks, local_task_budget, false);
    (void)move_posted_tasks(local_tasks);

    kqueue_operation_base* operations = reverse_tasks(local_tasks.pop_all());
    if (operations == nullptr) {
      break;
    }
    execute_tasks(operations);
  }
  state_.store(context_state::finished, std::memory_order_release);
}

}  // namespace bupp::async_io::bsd_native
