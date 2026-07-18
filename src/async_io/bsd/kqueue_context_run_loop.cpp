#include <chrono>
#include <limits>

#include "kqueue_context_internal.h"

namespace bnio::async_io::bsd_native {

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

  if (consume_local_state()) {
    return run_phase::run_ready_tasks;
  }
  if (consume_global_state()) {
    return run_phase::run_ready_tasks;
  }
  return should_finish() ? run_phase::finish_drain : run_phase::wait_for_work;
}

bool kqueue_context::consume_local_state() noexcept {
  if (kqueue_operation_base* operations =
          reverse_tasks(local_state_.cpu.pop_all())) {
    execute_tasks(operations);
    return true;
  }
  return consume_io_tasks();
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
    if (collect_ready_events(false) || consume_global_state()) {
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

  if (collect_ready_events(false) || consume_global_state() ||
      consume_local_state() || should_finish()) {
    end_wait();
    return should_finish() ? run_phase::finish_drain
                           : run_phase::run_ready_tasks;
  }

  timespec timeout{};
  const timespec* timeout_pointer = nullptr;
  if (global_state_ != nullptr && global_state_->timeout_heap != nullptr &&
      global_state_->try_fetch_timeout_operations != nullptr) {
    async_io::time_point deadline{};
    bool fetched_timeout_operations = false;
    if (global_state_->try_fetch_timeout_operations(
            global_state_->timeout_heap, deadline,
            fetched_timeout_operations)) {
      if (fetched_timeout_operations || consume_global_state() ||
          consume_local_state() || should_finish()) {
        end_wait();
        return should_finish() ? run_phase::finish_drain
                               : run_phase::run_ready_tasks;
      }
      if (deadline != async_io::time_point::max()) {
        const auto remaining = std::max(deadline - async_io::clock::now(),
                                        async_io::duration::zero());
        const auto nanoseconds =
            std::chrono::ceil<std::chrono::nanoseconds>(remaining);
        constexpr auto billion = std::chrono::nanoseconds::period::den;
        timeout.tv_sec = static_cast<time_t>(nanoseconds.count() / billion);
        timeout.tv_nsec = static_cast<long>(nanoseconds.count() % billion);
        timeout_pointer = &timeout;
      }
    }
  }

  const bool collected_events = collect_ready_events(true, timeout_pointer);
  end_wait();

  // A timeout is only a reason to become running. The normal ready phase
  // performs another complete work/timer decision before this worker sleeps.
  if (collected_events || timeout_pointer != nullptr ||
      consume_global_state() || consume_local_state()) {
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
    (void)consume_global_state();
    (void)collect_ready_events(false);
    (void)consume_global_state();

    if (!consume_local_state()) {
      break;
    }
  }
  state_.store(context_state::finished, std::memory_order_release);
}

}  // namespace bnio::async_io::bsd_native
