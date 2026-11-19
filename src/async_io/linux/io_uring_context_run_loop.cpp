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
 * @brief Fills timeout with the remaining time until deadline (zero if past).
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

bool io_uring_context::enter_run() noexcept {
  bool expected_active = false;
  if (!run_state_.run_active.compare_exchange_strong(
          expected_active, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }
  // Only the CAS winner checks state; losers must not assert on a value left
  // by a previous run cycle (a stop may leave the state non-running).
  assert_running();

  if (!is_open()) {
    run_state_.run_active.store(false, std::memory_order_release);
    return false;
  }

  // With SINGLE_ISSUER | R_DISABLED this makes the run-loop thread the
  // designated issuer before any SQE is submitted.
  const int enable_result = enable_ring();
  if (enable_result < 0) {
    // enable_ring() runs before awake_workers is incremented below.
    enter_run_error_ = enable_result;
    return fail_enter_run(/*awake_worker_added=*/false);
  }

  current_context_ = this;
  run_state_.waiting.store(false, std::memory_order_release);
  global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  // Arm both wake polls; failing either carries the same stranding risk as
  // the enable_ring() failure above.
  const int shared_poll_result =
      arm_wake_poll(global_state_->wake_channel_.read_fd(), eventfd_user_data(),
                    poll_state_.eventfd_poll_pending);
  if (shared_poll_result < 0) {
    enter_run_error_ = shared_poll_result;
    return fail_enter_run(/*awake_worker_added=*/true);
  }
  const int local_poll_result = arm_wake_poll(
      local_state_.wake_channel_.read_fd(), local_eventfd_user_data(),
      poll_state_.local_eventfd_poll_pending);
  if (local_poll_result < 0) {
    enter_run_error_ = local_poll_result;
    return fail_enter_run(/*awake_worker_added=*/true);
  }

  return true;
}

bool io_uring_context::fail_enter_run(bool awake_worker_added) noexcept {
  // run() must not return while posted operations still owe terminal calls:
  // deliver them through finish() (receivers run on this thread) before
  // bailing out. finish() tolerates the broken ring.
  run_state_.state.store(context_state::finishing, std::memory_order_release);
  finish();
  if (awake_worker_added) {
    global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
  }
  run_state_.run_active.store(false, std::memory_order_release);
  return false;
}

void io_uring_context::run() noexcept {
  io_uring_context* previous_context = current_context_;
  if (!enter_run()) {
    current_context_ = previous_context;
    return;
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

  // Unregister the local state before it is destroyed, so a concurrent
  // directed wakeup can never touch a dangling local_state.
  unregister_local_state();

  current_context_ = previous_context;
  global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
  run_state_.run_active.store(false, std::memory_order_release);
}

int io_uring_context::stop() noexcept {
  context_state expected = context_state::running;
  if (!run_state_.state.compare_exchange_strong(
          expected, context_state::finishing, std::memory_order_acq_rel,
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
  if (global_state_->timeout_heap == nullptr ||
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

  local_state_.push_cpu(operations);
  return true;
}

io_uring_context::run_phase
io_uring_context::handle_run_ready_tasks() noexcept {
  scheduling_state_.local_task_budget = options_.local_queue_threshold;

  // Drain CQEs first to keep the ring backlog small under load.
  (void)collect_ready_cqes();

  // One CPU batch (local -> shared) per pass bounds this thread's stack use.
  if (run_cpu_batch()) {
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
  // Bounded busy-spin: check CQEs, CPU tasks, and timers up to
  // wait_spin_count times to avoid the syscall cost of a quick completion.
  for (unsigned round = 0; round < options_.wait_spin_count; ++round) {
    if (collect_ready_cqes() || run_cpu_batch() ||
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
  // Mark this worker sleeping so publishers can wake it via eventfd.
  begin_wait();

  if (collect_pending_work() || should_finish()) {
    end_wait();
    return should_finish() ? run_phase::finish_drain
                           : run_phase::run_ready_tasks;
  }

  __kernel_timespec timeout{};
  __kernel_timespec* timeout_pointer = nullptr;
  const run_phase timeout_result =
      prepare_wait_timeout(timeout, timeout_pointer);
  if (timeout_result != run_phase::wait_for_work) {
    end_wait();
    return timeout_result;
  }

  switch (rearm_wake_polls_for_wait()) {
    case run_phase::run_ready_tasks:
      end_wait();
      return run_phase::run_ready_tasks;
    case run_phase::finish_drain:
      end_wait();
      return run_phase::finish_drain;
    case run_phase::wait_for_work:
      break;
    case run_phase::finished:
      // Unreachable: rearm_wake_polls_for_wait() never returns finished.
      break;
  }

  // Invariant: io_uring_enter below is never entered unbounded without a
  // wake source — an armed eventfd poll, a timeout, or inflight kernel I/O
  // being grace-waited.
  const int wait_result = wait_for_cqe_event(timeout_pointer);
  end_wait();

  // -EINTR is a spurious wakeup (e.g. a signal-driven stop()), never fatal;
  // it falls through to the re-evaluation below.
  if (wait_result < 0 && wait_result != -ETIME && wait_result != -EINTR &&
      !should_finish()) {
    // Fatal ring error: go through the finish drain so queued operations
    // reach terminal receiver calls.
    return run_phase::finish_drain;
  }

  if (collect_pending_work()) {
    return run_phase::run_ready_tasks;
  }

  if (timeout_pointer != nullptr) {
    return run_phase::run_ready_tasks;
  }

  return should_finish() ? run_phase::finish_drain : run_phase::wait_for_work;
}

bool io_uring_context::collect_pending_work() noexcept {
  return collect_ready_cqes() || run_cpu_batch() ||
         consume_timeout_operations() || consume_io_tasks();
}

io_uring_context::run_phase
io_uring_context::rearm_wake_polls_for_wait() noexcept {
  // Arms both wake polls; 1 = armed, 0 = stopping, <0 = submit failure.
  const int poll_result =
      arm_wake_poll(global_state_->wake_channel_.read_fd(), eventfd_user_data(),
                    poll_state_.eventfd_poll_pending);
  if (poll_result < 0) {
    if (poll_result == -EAGAIN) {
      // Transient SQ pressure: the poll is not armed, so never block here;
      // retry the collect/submit cycle.
      return run_phase::run_ready_tasks;
    }
    // Fatal re-arm failure: go through the finish drain so operations reach
    // terminal receiver calls.
    return run_phase::finish_drain;
  }
  const int local_poll_result = arm_wake_poll(
      local_state_.wake_channel_.read_fd(), local_eventfd_user_data(),
      poll_state_.local_eventfd_poll_pending);
  if (local_poll_result < 0) {
    if (local_poll_result == -EAGAIN) {
      // Transient SQ pressure; see the -EAGAIN branch above.
      return run_phase::run_ready_tasks;
    }
    // Fatal re-arm failure; see the finish_drain branch above.
    return run_phase::finish_drain;
  }
  // A skipped poll (0) is safe to block on only with another wake source;
  // re-check should_finish() to close the race with the last completion.
  if ((poll_result == 0 || local_poll_result == 0) && should_finish()) {
    return run_phase::finish_drain;
  }
  return run_phase::wait_for_work;
}

io_uring_context::run_phase io_uring_context::prepare_wait_timeout(
    __kernel_timespec& timeout, __kernel_timespec*& timeout_pointer) noexcept {
  // No timer heap configured: block without a deadline.
  if (global_state_->timeout_heap == nullptr ||
      global_state_->try_fetch_timeout_operations == nullptr) {
    return run_phase::wait_for_work;
  }

  async_io::time_point deadline{};
  io_uring_operation_base* timeout_operations = nullptr;
  if (!global_state_->try_fetch_timeout_operations(
          global_state_->timeout_heap, deadline, timeout_operations)) {
    return run_phase::wait_for_work;
  }

  if (timeout_operations != nullptr) {
    local_state_.push_cpu(timeout_operations);
  }
  // Runs while the worker is marked sleeping (only from wait_for_io_work()).
  if (timeout_operations != nullptr || run_cpu_batch() || consume_io_tasks() ||
      should_finish()) {
    return should_finish() ? run_phase::finish_drain
                           : run_phase::run_ready_tasks;
  }

  // No work found; arm the blocking wait with the nearest timer deadline.
  if (deadline != async_io::time_point::max()) {
    compute_wait_timespec(deadline, timeout);
    timeout_pointer = &timeout;
  }
  return run_phase::wait_for_work;
}

bool io_uring_context::closing_requested() const noexcept {
  return global_state_->life_state.load(std::memory_order_acquire) != 0;
}

bool io_uring_context::stop_requested() const noexcept {
  return run_state_.state.load(std::memory_order_acquire) !=
         context_state::running;
}

bool io_uring_context::should_finish() const noexcept {
  // Closing forces finish regardless of inflight. A normal stop() waits for
  // every inflight I/O to produce its CQE, so finish() cannot race them.
  if (closing_requested()) {
    return true;
  }
  if (stop_requested()) {
    return inflight_io_head_ == nullptr;
  }
  return false;
}

void io_uring_context::drain_local_tasks(bool include_cqe) noexcept {
  for (;;) {
    if (include_cqe) {
      (void)collect_ready_cqes();
    }
    (void)consume_timeout_operations();
    if (!run_cpu_batch()) {
      break;
    }
  }
}

void io_uring_context::abort_and_deliver_completions() noexcept {
  // Only a forced close reaches here with inflight I/O (stop() waits for the
  // list to empty); also drains shared I/O left by a run()-less teardown.
  abort_inflight_io();

  // Execute abort completions, plus timer abort ops that arrived after the
  // caller's first drain.
  drain_local_tasks(/*include_cqe=*/false);

  // set_stopped() receivers may publish new I/O on this thread, landing in
  // the local queue; repeat until no further I/O appears.
  while (consume_io_tasks()) {
    abort_inflight_io();
    drain_local_tasks(/*include_cqe=*/false);
  }
}

void io_uring_context::finish() noexcept {
  // Deliver every completion, tolerating a broken ring or closed wake channel,
  // so no operation is silently dropped.
  drain_local_tasks(/*include_cqe=*/true);

  abort_and_deliver_completions();

  run_state_.state.store(context_state::finished, std::memory_order_release);
}

}  // namespace bnio::async_io::linux_native
