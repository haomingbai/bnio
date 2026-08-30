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

  // With SINGLE_ISSUER | R_DISABLED, this call makes the run-loop thread the
  // designated issuer before any SQE can be submitted.
  if (enable_ring() < 0) {
    // run() must not return while operations already posted to the
    // shared queues still owe their receivers a terminal call. Deliver
    // them through finish()'s abort path (receivers run on this, the
    // run()-caller, thread) before bailing out. finish() tolerates the
    // broken ring: collect_ready_cqes() and consume_io_tasks() guard the
    // ring state and fail prepared operations through delivery.
    run_state_.state.store(context_state::finishing, std::memory_order_release);
    finish();
    run_state_.run_active.store(false, std::memory_order_release);
    return false;
  }

  current_context_ = this;
  run_state_.waiting.store(false, std::memory_order_release);
  global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  const int poll_result =
      arm_wake_poll(global_state_->wake_channel_.read_fd(), eventfd_user_data(),
                    poll_state_.eventfd_poll_pending);
  if (poll_result < 0) {
    // same stranding risk as the enable_ring failure above.
    run_state_.state.store(context_state::finishing, std::memory_order_release);
    finish();
    global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
    run_state_.run_active.store(false, std::memory_order_release);
    return false;
  }

  // Arm the per-worker wake channel for directed wakeups.
  if (arm_wake_poll(local_state_.wake_channel_.read_fd(),
                    local_eventfd_user_data(),
                    poll_state_.local_eventfd_poll_pending) < 0) {
    // Same stranding risk as the enable_ring failure above.
    run_state_.state.store(context_state::finishing, std::memory_order_release);
    finish();
    global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
    run_state_.run_active.store(false, std::memory_order_release);
    return false;
  }

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

  // Unregister the local state (from the suspend list, if the worker still
  // sits there) before the context and its local state are destroyed, so a
  // concurrent directed wakeup can never touch a dangling local_state.
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
  // Reset the local-queue budget for this pass through the run loop.
  scheduling_state_.local_task_budget = options_.local_queue_threshold;

  // Always drain CQEs first to keep the ring backlog small under load.
  (void)collect_ready_cqes();

  // Run one batch of CPU tasks, trying local → shared in that order.
  // Stopping after one batch keeps work from piling up on this thread's
  // stack; the shared queue distributes the rest fairly.
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

  if (collect_ready_cqes() || run_cpu_batch() ||
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
  // always wake this worker. Return codes: 1 = armed (newly or already
  // pending), 0 = not armed because the context is stopping, negative =
  // submission failure.
  const int poll_result =
      arm_wake_poll(global_state_->wake_channel_.read_fd(), eventfd_user_data(),
                    poll_state_.eventfd_poll_pending);
  if (poll_result < 0) {
    end_wait();
    if (poll_result == -EAGAIN) {
      // Transient SQ pressure (typically SQPOLL, where only the kernel
      // poll thread frees SQ slots): the poll is not armed, so never
      // block here. Retry the collect/submit cycle; the kernel consumes
      // the queued SQEs and a later pass re-arms successfully.
      return run_phase::run_ready_tasks;
    }
    // Fatal re-arm failure (e.g. the wake channel was closed): route
    // through the finish drain so inflight and queued operations reach
    // terminal receiver calls instead of being stranded.
    return run_phase::finish_drain;
  }
  const int local_poll_result = arm_wake_poll(
      local_state_.wake_channel_.read_fd(), local_eventfd_user_data(),
      poll_state_.local_eventfd_poll_pending);
  if (local_poll_result < 0) {
    end_wait();
    if (local_poll_result == -EAGAIN) {
      // Transient SQ pressure; see the -EAGAIN branch above.
      return run_phase::run_ready_tasks;
    }
    // Fatal re-arm failure; see the finish_drain branch above.
    return run_phase::finish_drain;
  }
  // A 0 return means a poll was skipped because the context is stopping.
  // Blocking is then allowed only under graceful-stop semantics: the
  // should_finish() checks above ran with inflight I/O whose CQEs will
  // wake this worker (or a timeout bounds the wait). Re-evaluate it here
  // so the last inflight completion racing between those checks and the
  // re-arm can never leave the worker blocked without a wake source.
  if ((poll_result == 0 || local_poll_result == 0) && should_finish()) {
    end_wait();
    return run_phase::finish_drain;
  }

  // Invariant: io_uring_enter below is never entered unbounded without a
  // wake source — an eventfd poll is armed, a timeout bounds the wait, or
  // inflight kernel operations are being grace-waited.
  const int wait_result = wait_for_cqe_event(timeout_pointer);
  end_wait();

  // -EINTR is a spurious wakeup (e.g. SIGWINCH/SIGCHLD or a signal-driven
  // stop()): never fatal. Fall through to the collect + state re-evaluation
  // logic below, which either finishes (stop requested) or waits again.
  if (wait_result < 0 && wait_result != -ETIME && wait_result != -EINTR &&
      !should_finish()) {
    // Fatal ring error: route through the finish drain so inflight and
    // queued operations reach terminal receiver calls instead of being
    // stranded by a direct exit to finished.
    return run_phase::finish_drain;
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
  // wait_for_io_work() after begin_wait()).
  if (timeout_operations != nullptr || run_cpu_batch() ||
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

void io_uring_context::abort_and_deliver_completions() noexcept {
  // Phase 2: safety net for abnormal shutdown (closing flag) where
  // inflight I/O was not fully drained by the run loop.  Normal
  // stop()-driven shutdown never reaches here with inflight operations
  // because should_finish() waits for inflight_io_head_ == nullptr.
  // This also aborts anything left in the shared I/O queue (e.g. a
  // queue_exit() on a context whose run() never drained it).
  abort_inflight_io();

  // Phase 3: execute CPU tasks generated by the abort phase, plus any
  // timer abort ops that arrived after the caller's first drain (and,
  // when entered from queue_exit(), operations that were posted without
  // an intervening run()).
  drain_local_tasks(/*include_cqe=*/false);

  // Phase 3b: drain any new I/O operations generated by Phase 3
  // callbacks — and any further I/O operations those in turn generate.
  // When set_stopped() propagates through receiver chains, custom
  // receivers may start new I/O via publish_io(). They run on this
  // thread inside this context, so publish_io() takes its worker-local
  // fast path and the operations land in the local I/O queue — which
  // Phase 3 above never drains.  We loop
  // consume_io_tasks() → abort_inflight_io() → drain until no more I/O
  // tasks appear, closing the nested-publish window.
  while (consume_io_tasks()) {
    abort_inflight_io();
    drain_local_tasks(/*include_cqe=*/false);
  }
}

void io_uring_context::finish() noexcept {
  // Phase 1: drain already-ready CQEs, CPU tasks, and timer abort ops.
  // Tolerates a broken or closed ring and wake channel: every step
  // guards its channel/ring state and fails operations through delivery
  // (complete_submit_error / complete_submit_stopped), so no path
  // silently drops a completion.
  drain_local_tasks(/*include_cqe=*/true);

  // Phases 2–3b: abort remaining I/O and deliver every completion, so
  // each operation reaches a terminal receiver call (I1) even when
  // finish() is entered from a fatal re-arm or wait error.
  abort_and_deliver_completions();

  run_state_.state.store(context_state::finished, std::memory_order_release);
}

}  // namespace bnio::async_io::linux_native
