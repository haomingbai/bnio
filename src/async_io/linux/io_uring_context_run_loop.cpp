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

bool io_uring_context::enter_run() noexcept {
  bool expected_active = false;
  if (!run_state_.run_active.compare_exchange_strong(
          expected_active, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }
  // Only the thread that wins the CAS checks state.  Concurrent callers
  // that lose the race return immediately — they must not assert on a
  // run_state_.state value left by a previous run cycle.  See
  // stop_internal() ordering guarantee in io_context::begin_stop().
  assert_running();

  if (!is_open()) {
    run_state_.run_active.store(false, std::memory_order_release);
    return false;
  }

  assert(global_state_ != nullptr &&
         "set_global_state() must be called before run()");

  // With SINGLE_ISSUER | R_DISABLED, this call makes the run-loop thread the
  // designated issuer before any SQE can be submitted.
  if (enable_ring() < 0) {
    run_state_.state.store(context_state::finished, std::memory_order_release);
    run_state_.run_active.store(false, std::memory_order_release);
    return false;
  }

  current_context_ = this;
  run_state_.waiting.store(false, std::memory_order_release);
  global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  const int poll_result = submit_eventfd_poll();
  if (poll_result < 0) {
    run_state_.state.store(context_state::finished, std::memory_order_release);
    global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
    run_state_.run_active.store(false, std::memory_order_release);
    return false;
  }

  // Arm the per-worker wake channel for directed wakeups and publish this
  // worker's local state so remote threads can steal CPU work.
  if (submit_local_eventfd_poll() < 0) {
    run_state_.state.store(context_state::finished, std::memory_order_release);
    global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
    run_state_.run_active.store(false, std::memory_order_release);
    return false;
  }
  register_local_state();

  return true;
}

void io_uring_context::run() noexcept {
  io_uring_context* previous_context = current_context_;
  if (!enter_run()) {
    current_context_ = previous_context;
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

  // Unregister before the context (and its local state) is destroyed so a
  // concurrent stealer can never observe a dangling local_state.
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

  local_state_.push_cpu(operations);
  return true;
}

io_uring_context::run_phase
io_uring_context::handle_run_ready_tasks() noexcept {
  // Reset the local-queue budget for this pass through the run loop.
  scheduling_state_.local_task_budget = options_.local_queue_threshold;

  // Always drain CQEs first to keep the ring backlog small under load.
  (void)collect_ready_cqes();

  // Run one batch of CPU tasks, trying local → shared → steal in that
  // order. Stopping after one batch keeps work from piling up on this
  // thread's stack, so other workers can steal it instead.
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
  // Bounded busy-spin: check for new CQEs, CPU tasks, and expired timers up
  // to wait_spin_count times before falling through to a blocking wait.
  // This avoids the syscall cost when completions arrive quickly.
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
  // Wrap the wait section with begin_wait/end_wait so task-queue
  // publishers can detect a sleeping worker and wake it via eventfd.
  begin_wait();

  if (collect_ready_cqes() || run_cpu_batch(/*allow_steal=*/false) ||
      consume_timeout_operations() || consume_io_tasks() || should_finish()) {
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

  // Rearm both wake polls before blocking so a concurrent publisher can
  // always wake this worker.
  const int poll_result = submit_eventfd_poll();
  if (poll_result < 0) {
    end_wait();
    run_state_.state.store(context_state::finished, std::memory_order_release);
    return run_phase::finished;
  }
  if (submit_local_eventfd_poll() < 0) {
    end_wait();
    run_state_.state.store(context_state::finished, std::memory_order_release);
    return run_phase::finished;
  }
  const int wait_result = wait_for_cqe_event(timeout_pointer);
  end_wait();

  // -EINTR is a spurious wakeup (e.g. SIGWINCH/SIGCHLD or a signal-driven
  // stop()): never fatal. Fall through to the collect + state re-evaluation
  // logic below, which either finishes (stop requested) or waits again.
  if (wait_result < 0 && wait_result != -ETIME && wait_result != -EINTR &&
      !should_finish()) {
    run_state_.state.store(context_state::finished, std::memory_order_release);
    return run_phase::finished;
  }

  if (collect_ready_cqes() || run_cpu_batch() || consume_timeout_operations() ||
      consume_io_tasks()) {
    return run_phase::run_ready_tasks;
  }

  if (timeout_pointer != nullptr) {
    return run_phase::run_ready_tasks;
  }

  return should_finish() ? run_phase::finish_drain : run_phase::wait_for_work;
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
  // Runs while the worker is marked sleeping (only called from
  // wait_for_io_work() after begin_wait()), so skip the run-list steal —
  // the pre-sleep steal already found nothing, and taking run.lock here
  // would just add contention.
  if (timeout_operations != nullptr || run_cpu_batch(/*allow_steal=*/false) ||
      consume_io_tasks() || should_finish()) {
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
  // Abnormal shutdown (closing flag): force finish regardless of inflight.
  if (closing_requested()) {
    return true;
  }
  // Normal stop(): wait until every inflight I/O has produced its CQE.
  // This guarantees finish() never races with in-flight completions.
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

void io_uring_context::finish() noexcept {
  // Phase 1: drain already-ready CQEs, CPU tasks, and timer abort ops.
  drain_local_tasks(/*include_cqe=*/true);

  // Phase 2: safety net for abnormal shutdown (closing flag) where
  // inflight I/O was not fully drained by the run loop.  Normal
  // stop()-driven shutdown never reaches here with inflight operations
  // because should_finish() waits for inflight_io_head_ == nullptr.
  abort_inflight_io();

  // Phase 3: execute CPU tasks generated by the abort phase, plus any
  // timer abort ops that arrived after Phase 1.
  drain_local_tasks(/*include_cqe=*/false);

  // Phase 3b: drain any new I/O operations generated by Phase 3
  // callbacks — and any further I/O operations those in turn generate.
  // When set_stopped() propagates through receiver chains, custom
  // receivers may start new I/O via publish_io(), which pushes to the
  // shared global I/O queue.  Phase 3 above only drains the local task
  // queue, so those new I/O tasks would be stranded.  We loop
  // consume_io_tasks() → abort_inflight_io() → drain until no more I/O
  // tasks appear, closing the nested-publish window.
  while (consume_io_tasks()) {
    abort_inflight_io();
    drain_local_tasks(/*include_cqe=*/false);
  }

  run_state_.state.store(context_state::finished, std::memory_order_release);
}

}  // namespace bnio::async_io::linux_native
