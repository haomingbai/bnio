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

  kqueue_context* previous_context = current_context_;
  current_context_ = this;
  waiting_.store(false, std::memory_order_release);
  if (global_state_ != nullptr) {
    global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  }

  run_phase phase = run_phase::run_ready_tasks;
  while (phase != run_phase::finished) {
    switch (phase) {
      case run_phase::run_ready_tasks:
        phase = handle_run_ready_tasks();
        break;
      case run_phase::wait_for_work:
        phase = handle_wait_for_work();
        break;
      case run_phase::finish_drain:
        phase = handle_finish_drain();
        break;
      case run_phase::finished:
        break;
    }
  }

  current_context_ = previous_context;
  if (global_state_ != nullptr) {
    global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
  }
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

kqueue_context::run_phase kqueue_context::handle_run_ready_tasks() noexcept {
  local_task_budget_ = options_.local_queue_threshold;

  if (kqueue_operation_base* operations =
          reverse_tasks(local_tasks_.pop_all())) {
    execute_tasks(operations);
    return run_phase::run_ready_tasks;
  }
  if (move_cpu_tasks()) {
    return run_phase::run_ready_tasks;
  }
  if (consume_io_tasks()) {
    return run_phase::run_ready_tasks;
  }
  return should_finish() ? run_phase::finish_drain : run_phase::wait_for_work;
}

kqueue_context::run_phase kqueue_context::handle_wait_for_work() noexcept {
  const run_phase spin_result = spin_for_work();
  if (spin_result != run_phase::wait_for_work) {
    return spin_result;
  }
  return wait_for_io_work();
}

kqueue_context::run_phase kqueue_context::handle_finish_drain() noexcept {
  finish();
  return run_phase::finished;
}

kqueue_context::run_phase kqueue_context::spin_for_work() noexcept {
  for (unsigned round = 0; round < options_.wait_spin_count; ++round) {
    if (collect_ready_events(false) || move_cpu_tasks()) {
      return run_phase::run_ready_tasks;
    }
    if (should_finish()) {
      return run_phase::finish_drain;
    }
  }
  return run_phase::wait_for_work;
}

kqueue_context::run_phase kqueue_context::wait_for_io_work() noexcept {
  begin_wait();

  if (collect_ready_events(false) || move_cpu_tasks() || consume_io_tasks() ||
      should_finish()) {
    end_wait();
    return should_finish() ? run_phase::finish_drain
                           : run_phase::run_ready_tasks;
  }

  const bool collected_events = collect_ready_events(true);
  end_wait();

  if (collected_events || move_cpu_tasks() || consume_io_tasks()) {
    return run_phase::run_ready_tasks;
  }
  return should_finish() ? run_phase::finish_drain : run_phase::wait_for_work;
}

bool kqueue_context::should_finish() const noexcept {
  return state_.load(std::memory_order_acquire) != context_state::running ||
         (global_state_ != nullptr &&
          global_state_->closing.load(std::memory_order_acquire));
}

void kqueue_context::finish() noexcept {
  for (;;) {
    (void)move_cpu_tasks();
    (void)collect_ready_events(false);
    (void)move_cpu_tasks();

    kqueue_operation_base* operations = reverse_tasks(local_tasks_.pop_all());
    if (operations == nullptr) {
      break;
    }
    execute_tasks(operations);
  }
  state_.store(context_state::finished, std::memory_order_release);
}

}  // namespace bupp::async_io::bsd_native
