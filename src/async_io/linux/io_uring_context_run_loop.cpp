#include <cassert>

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

  assert(global_state_ != nullptr &&
         "set_global_state() must be called before run()");

  io_uring_context* previous_context = current_context_;
  current_context_ = this;
  waiting_.store(false, std::memory_order_release);
  global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  (void)submit_eventfd_poll();

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
  global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
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

io_uring_context::run_phase
io_uring_context::handle_run_ready_tasks() noexcept {
  // Reset the local-queue budget for this pass through the run loop.
  local_task_budget_ = options_.local_queue_threshold;

  // Always drain CQEs first to keep the ring backlog small under load.
  (void)collect_ready_cqes();

  if (io_uring_operation_base* operations = local_tasks_.pop_all()) {
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

io_uring_context::run_phase io_uring_context::handle_wait_for_work() noexcept {
  const run_phase spin_result = spin_for_work();
  if (spin_result != run_phase::wait_for_work) {
    return spin_result;
  }

  return wait_for_io_work();
}

io_uring_context::run_phase io_uring_context::handle_finish_drain() noexcept {
  finish();
  return run_phase::finished;
}

io_uring_context::run_phase io_uring_context::spin_for_work() noexcept {
  for (unsigned round = 0; round < options_.wait_spin_count; ++round) {
    if (collect_ready_cqes() || move_cpu_tasks()) {
      return run_phase::run_ready_tasks;
    }
    if (should_finish()) {
      return run_phase::finish_drain;
    }
  }

  return run_phase::wait_for_work;
}

io_uring_context::run_phase io_uring_context::wait_for_io_work() noexcept {
  begin_wait();

  if (collect_ready_cqes() || move_cpu_tasks() || consume_io_tasks() ||
      should_finish()) {
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

  if (collect_ready_cqes() || move_cpu_tasks() || consume_io_tasks()) {
    return run_phase::run_ready_tasks;
  }

  return should_finish() ? run_phase::finish_drain : run_phase::wait_for_work;
}

bool io_uring_context::should_finish() const noexcept {
  return state_.load(std::memory_order_acquire) != context_state::running ||
         global_state_->closing.load(std::memory_order_acquire);
}

void io_uring_context::finish() noexcept {
  for (;;) {
    (void)move_cpu_tasks();
    (void)collect_ready_cqes();
    (void)move_cpu_tasks();

    io_uring_operation_base* operations = local_tasks_.pop_all();
    if (operations == nullptr) {
      break;
    }
    execute_tasks(operations);
  }

  state_.store(context_state::finished, std::memory_order_release);
}

}  // namespace bupp::async_io::linux_native
