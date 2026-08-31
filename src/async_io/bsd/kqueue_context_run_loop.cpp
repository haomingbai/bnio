/**
 * @file kqueue_context_run_loop.cpp
 * @brief kqueue_context event loop: run, spin, wait, and finish phases.
 */

#include <chrono>
#include <limits>

#include "kqueue_context_internal.h"

namespace bnio::async_io::bsd_native {
namespace {

/**
 * @brief Fills a timespec with the remaining duration until deadline.
 *
 * @param deadline The deadline time point to compute against.
 * @param timeout  The timespec to fill with the remaining duration in seconds
 * and nanoseconds.
 */
void compute_wait_timespec(async_io::time_point deadline,
                           timespec& timeout) noexcept {
  const auto remaining =
      std::max(deadline - async_io::clock::now(), async_io::duration::zero());
  const auto nanoseconds =
      std::chrono::ceil<std::chrono::nanoseconds>(remaining);
  constexpr auto billion = std::chrono::nanoseconds::period::den;
  timeout.tv_sec = static_cast<time_t>(nanoseconds.count() / billion);
  timeout.tv_nsec = static_cast<long>(nanoseconds.count() % billion);
}

}  // namespace

void kqueue_context::register_wake_poll(int fd, void* udata) noexcept {
  // EV_CLEAR makes the event edge-triggered: it re-fires only on the next
  // write after the channel is drained. `udata` tags the event so
  // process_event() filters it out of operation dispatch.
  bnio::base::event wake_event(static_cast<std::uintptr_t>(fd), EVFILT_READ,
                               EV_ADD | EV_CLEAR, 0, 0, udata);
  (void)queue_.control(&wake_event, 1, nullptr, 0, nullptr);
}

bool kqueue_context::enter_run() noexcept {
  bool expected_active = false;
  if (!run_state_.run_active.compare_exchange_strong(
          expected_active, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }
  // Only the thread that wins the CAS checks state.  Concurrent callers
  // that lose the race return immediately — they must not assert on a
  // run_state_.state value left by a previous run cycle.
  assert_running();

  if (!is_open()) {
    run_state_.run_active.store(false, std::memory_order_release);
    return false;
  }

  current_context_ = this;
  run_state_.waiting.store(false, std::memory_order_release);
  if (global_state_ != nullptr) {
    global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  }

  // Register the shared wake fd so io_context can wake this worker via a
  // write to the shared wake channel.
  if (global_state_ != nullptr && global_state_->wake_channel_.is_open()) {
    register_wake_poll(global_state_->wake_channel_.read_fd(),
                       wakeup_user_data());
  }

  // Register the per-worker wake channel so a directed wake can target
  // this worker without waking the whole group.
  if (local_state_.wake_channel_.is_open()) {
    register_wake_poll(local_state_.wake_channel_.read_fd(),
                       local_wakeup_user_data());
  }

  return true;
}

void kqueue_context::run() noexcept {
  kqueue_context* previous_context = current_context_;
  if (!enter_run()) {
    current_context_ = previous_context;
    return;
  }

  run_phase phase = run_phase::run_ready_tasks;
  // Run-loop phase machine:
  //   run_ready_tasks -> wait_for_work -> finish_drain -> finished
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
  if (global_state_ != nullptr) {
    global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
  }
  run_state_.run_active.store(false, std::memory_order_release);
}

int kqueue_context::stop() noexcept {
  context_state expected = context_state::running;
  if (!run_state_.state.compare_exchange_strong(
          expected, context_state::finishing, std::memory_order_acq_rel,
          std::memory_order_acquire) &&
      expected != context_state::finishing) {
    return 0;
  }
  return trigger_wakeup();
}

bool kqueue_context::is_in_context() const noexcept {
  return current_context_ == this;
}

bool kqueue_context::consume_timeout_operations() noexcept {
  if (global_state_ == nullptr || global_state_->timeout_heap == nullptr ||
      global_state_->try_fetch_timeout_operations == nullptr) {
    return false;
  }

  async_io::time_point deadline{};
  kqueue_operation_base* operations = nullptr;
  if (!global_state_->try_fetch_timeout_operations(global_state_->timeout_heap,
                                                   deadline, operations) ||
      operations == nullptr) {
    return false;
  }

  local_state_.push_cpu(operations);
  return true;
}

kqueue_context::run_phase kqueue_context::handle_run_ready_tasks() noexcept {
  scheduling_state_.local_task_budget = options_.local_queue_threshold;

  // 1. Poll ready kevents only when there is inflight I/O that could have
  //    produced a completion. Without inflight I/O, kevent() would always
  //    return 0 events — the syscall is pure overhead (notably on the
  //    timer-only path). Mirrors io_uring's collect_ready_cqes(), which reads
  //    a user-space ring and is cheap to call unconditionally; kqueue has no
  //    such user-space shortcut, so we gate on inflight_io_head_.
  if (inflight_io_head_ != nullptr) {
    (void)collect_ready_events(false);
  }

  // 2. Run one batch of CPU tasks, trying local → shared in that order.
  //    Stopping after one batch keeps work from piling up on this thread's
  //    stack; the shared queue distributes the rest fairly.
  if (run_cpu_batch()) {
    return run_phase::run_ready_tasks;
  }

  // 3. Consume timer expirations.
  if (consume_timeout_operations()) {
    return run_phase::run_ready_tasks;
  }

  // 4. Register pending I/O tasks with kqueue — always a separate step
  //    so repeat_until chains that generate I/O during CPU processing
  //    don't starve the kqueue filter set.  Matches Linux's explicit
  //    consume_io_tasks() call after CPU draining.
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
  // Bounded busy-spin: check for new events and CPU tasks up to
  // wait_spin_count times before falling through to kevent().
  for (unsigned round = 0; round < options_.wait_spin_count; ++round) {
    if (collect_ready_events(false) || run_cpu_batch() ||
        consume_timeout_operations()) {
      return run_phase::run_ready_tasks;
    }
    if (should_finish()) {
      return run_phase::finish_drain;
    }
  }
  return run_phase::wait_for_work;
}

kqueue_context::run_phase kqueue_context::wait_for_io_work() noexcept {
  // begin_wait/end_wait sandwich: allows task publishers to detect a sleeping
  // worker and wake it via EVFILT_USER trigger.
  begin_wait();

  if (collect_ready_events(false) || run_cpu_batch() ||
      consume_timeout_operations() || consume_io_tasks() || should_finish()) {
    end_wait();
    return should_finish() ? run_phase::finish_drain
                           : run_phase::run_ready_tasks;
  }

  timespec timeout{};
  const timespec* timeout_pointer = nullptr;
  async_io::time_point deadline{};
  if (compute_io_wait_timeout(deadline, timeout, timeout_pointer)) {
    end_wait();
    return should_finish() ? run_phase::finish_drain
                           : run_phase::run_ready_tasks;
  }

  const bool collected_events = collect_ready_events(true, timeout_pointer);
  end_wait();

  // A timeout is only a reason to become running. The normal ready phase
  // performs another complete work/timer decision before this worker sleeps.
  if (collected_events || timeout_pointer != nullptr || run_cpu_batch() ||
      consume_timeout_operations() || consume_io_tasks()) {
    return run_phase::run_ready_tasks;
  }
  return should_finish() ? run_phase::finish_drain : run_phase::wait_for_work;
}

bool kqueue_context::compute_io_wait_timeout(
    async_io::time_point& deadline, timespec& timeout,
    const timespec*& timeout_pointer) noexcept {
  timeout_pointer = nullptr;
  if (global_state_ == nullptr || global_state_->timeout_heap == nullptr ||
      global_state_->try_fetch_timeout_operations == nullptr) {
    return false;
  }

  kqueue_operation_base* timeout_operations = nullptr;
  if (!global_state_->try_fetch_timeout_operations(
          global_state_->timeout_heap, deadline, timeout_operations)) {
    return false;
  }

  if (timeout_operations != nullptr) {
    local_state_.push_cpu(timeout_operations);
  }
  // Runs while the worker is marked sleeping (only called from
  // wait_for_io_work() after begin_wait()).
  if (timeout_operations != nullptr || run_cpu_batch() || consume_io_tasks() ||
      should_finish()) {
    return true;
  }

  if (deadline != async_io::time_point::max()) {
    compute_wait_timespec(deadline, timeout);
    timeout_pointer = &timeout;
  }
  return false;
}

bool kqueue_context::closing_requested() const noexcept {
  return global_state_ != nullptr &&
         global_state_->life_state.load(std::memory_order_acquire) != 0;
}

bool kqueue_context::stop_requested() const noexcept {
  return run_state_.state.load(std::memory_order_acquire) !=
         context_state::running;
}

bool kqueue_context::should_finish() const noexcept {
  // Abnormal shutdown (closing flag): force finish regardless of inflight.
  if (closing_requested()) {
    return true;
  }
  // Normal stop(): wait until every inflight I/O has produced its event.
  // This guarantees finish() never races with in-flight completions.
  if (stop_requested()) {
    return inflight_io_head_ == nullptr;
  }
  return false;
}

void kqueue_context::drain_local_cpu_tasks() noexcept {
  for (;;) {
    (void)consume_timeout_operations();
    if (!run_cpu_batch()) {
      break;
    }
  }
}

void kqueue_context::finish() noexcept {
  // Phase 1: drain already-ready kevents, CPU tasks, and timer abort ops.
  for (;;) {
    (void)collect_ready_events(false);
    (void)consume_timeout_operations();
    if (!run_cpu_batch() && !consume_io_tasks()) break;
  }

  // Phase 2: safety net for abnormal shutdown (closing flag) where
  // inflight I/O was not fully drained by the run loop.  Normal
  // stop()-driven shutdown never reaches here with inflight operations
  // because should_finish() waits for inflight_io_head_ == nullptr.
  abort_inflight_io();

  // Phase 3: execute CPU tasks generated by the abort phase, plus any
  // timer abort ops that arrived after Phase 1.
  drain_local_cpu_tasks();

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
    drain_local_cpu_tasks();
  }

  run_state_.state.store(context_state::finished, std::memory_order_release);
}

}  // namespace bnio::async_io::bsd_native
