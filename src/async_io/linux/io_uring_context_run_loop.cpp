/**
 * @file io_uring_context_run_loop.cpp
 * @brief io_uring_context event loop: run, spin, wait, and finish phases.
 */

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>

#include "io_uring_context_internal.h"

namespace bnio::async_io::linux_native {
namespace {

/**
 * @brief Fills a __kernel_timespec with the remaining duration until deadline.
 *
 * @param deadline The deadline time point to compute against.
 * @param timeout  The timespec to fill with the remaining duration in seconds
 * and nanoseconds.
 */
void compute_wait_timespec(async_io::time_point deadline,
                           __kernel_timespec& timeout) noexcept {
  const auto remaining =
      std::max(deadline - async_io::clock::now(), async_io::duration::zero());
  const auto nanoseconds =
      std::chrono::ceil<std::chrono::nanoseconds>(remaining);
  constexpr auto billion = std::chrono::nanoseconds::period::den;
  timeout.tv_sec =
      static_cast<__kernel_time64_t>(nanoseconds.count() / billion);
  timeout.tv_nsec = static_cast<long long>(nanoseconds.count() % billion);
}

}  // namespace

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

  // With SINGLE_ISSUER | R_DISABLED, this call makes the run-loop thread the
  // designated issuer before any SQE can be submitted.
  if (enable_ring() < 0) {
    state_.store(context_state::finished, std::memory_order_release);
    run_active_.store(false, std::memory_order_release);
    return;
  }

  io_uring_context* previous_context = current_context_;
  current_context_ = this;
  waiting_.store(false, std::memory_order_release);
  global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  const int poll_result = submit_eventfd_poll();
  if (poll_result < 0) {
    state_.store(context_state::finished, std::memory_order_release);
    current_context_ = previous_context;
    global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
    run_active_.store(false, std::memory_order_release);
    return;
  }

  run_phase phase = run_phase::run_ready_tasks;

  // Run-loop phase machine:
  //   run_ready_tasks -> wait_for_work -> finish_drain -> finished
  // Each phase returns the next phase to enter. The loop stops at finished.
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

bool io_uring_context::consume_timeout_operations() noexcept {
  if (global_state_ == nullptr || global_state_->timeout_heap == nullptr ||
      global_state_->try_fetch_timeout_operations == nullptr) {
    return false;
  }

  async_io::time_point deadline{};
  io_uring_operation_base* operations = nullptr;
  if (!global_state_->try_fetch_timeout_operations(global_state_->timeout_heap,
                                                   deadline, operations) ||
      operations == nullptr) {
    return false;
  }

  local_tasks_.push(operations);
  return true;
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

  if (consume_timeout_operations()) {
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
  // Bounded busy-spin: check for new CQEs, CPU tasks, and expired timers up
  // to wait_spin_count times before falling through to a blocking wait.
  // This avoids the syscall cost when completions arrive quickly.
  for (unsigned round = 0; round < options_.wait_spin_count; ++round) {
    if (collect_ready_cqes() || move_cpu_tasks() ||
        consume_timeout_operations()) {
      return run_phase::run_ready_tasks;
    }
    if (should_finish()) {
      return run_phase::finish_drain;
    }
  }

  return run_phase::wait_for_work;
}

io_uring_context::run_phase io_uring_context::wait_for_io_work() noexcept {
  // Wrap the wait section with begin_wait/end_wait so task-queue
  // publishers can detect a sleeping worker and wake it via eventfd.
  begin_wait();

  if (collect_ready_cqes() || move_cpu_tasks() ||
      consume_timeout_operations() || consume_io_tasks() || should_finish()) {
    end_wait();
    return should_finish() ? run_phase::finish_drain
                           : run_phase::run_ready_tasks;
  }

  __kernel_timespec timeout{};
  __kernel_timespec* timeout_pointer = nullptr;
  if (global_state_->timeout_heap != nullptr &&
      global_state_->try_fetch_timeout_operations != nullptr) {
    async_io::time_point deadline{};
    io_uring_operation_base* timeout_operations = nullptr;
    if (global_state_->try_fetch_timeout_operations(
            global_state_->timeout_heap, deadline, timeout_operations)) {
      if (timeout_operations != nullptr) {
        local_tasks_.push(timeout_operations);
      }
      if (timeout_operations != nullptr || move_cpu_tasks() ||
          consume_io_tasks() || should_finish()) {
        end_wait();
        return should_finish() ? run_phase::finish_drain
                               : run_phase::run_ready_tasks;
      }

      if (deadline != async_io::time_point::max()) {
        compute_wait_timespec(deadline, timeout);
        timeout_pointer = &timeout;
      }
    }
  }

  const int poll_result = submit_eventfd_poll();
  if (poll_result < 0) {
    end_wait();
    state_.store(context_state::finished, std::memory_order_release);
    return run_phase::finished;
  }
  const int wait_result = wait_for_cqe_event(timeout_pointer);
  end_wait();

  if (wait_result < 0 && wait_result != -ETIME && !should_finish()) {
    return run_phase::finished;
  }

  if (collect_ready_cqes() || move_cpu_tasks() ||
      consume_timeout_operations() || consume_io_tasks()) {
    return run_phase::run_ready_tasks;
  }

  if (timeout_pointer != nullptr) {
    return run_phase::run_ready_tasks;
  }

  return should_finish() ? run_phase::finish_drain : run_phase::wait_for_work;
}

bool io_uring_context::should_finish() const noexcept {
  return state_.load(std::memory_order_acquire) != context_state::running ||
         global_state_->closing.load(std::memory_order_acquire);
}

void io_uring_context::finish() noexcept {
  // Phase 1: drain already-ready CQEs and CPU tasks.
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

  // Phase 2: abort all I/O still in-flight (submitted to ring but no
  // CQE yet) and drain unregistered I/O from the global queue.
  abort_inflight_io();

  // Phase 3: execute CPU tasks generated by the abort phase.
  for (;;) {
    (void)move_cpu_tasks();
    io_uring_operation_base* operations = local_tasks_.pop_all();
    if (operations == nullptr) {
      break;
    }
    execute_tasks(operations);
  }

  state_.store(context_state::finished, std::memory_order_release);
}

}  // namespace bnio::async_io::linux_native
