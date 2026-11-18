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
 * @brief Fills timeout with the remaining time until deadline (zero if past).
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
  // EV_CLEAR makes the event edge-triggered: it re-fires only after the
  // channel is drained. `udata` tags it for process_event().
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
  // Only the CAS winner checks state; losers must not assert on a value left
  // by a previous run cycle.
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

  // Register the shared wake fd so io_context can wake this worker.
  if (global_state_ != nullptr && global_state_->wake_channel_.is_open()) {
    register_wake_poll(global_state_->wake_channel_.read_fd(),
                       wakeup_user_data());
  }

  // Register the per-worker channel for directed wakeups.
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

  // Poll only when inflight I/O could have produced a completion: without
  // it, kevent() always returns 0 and is pure syscall overhead.
  if (inflight_io_head_ != nullptr) {
    (void)collect_ready_events(false);
  }

  // One CPU batch (local -> shared) per pass bounds this thread's stack use.
  if (run_cpu_batch()) {
    return run_phase::run_ready_tasks;
  }

  if (consume_timeout_operations()) {
    return run_phase::run_ready_tasks;
  }

  // Register pending I/O only after CPU draining, so repeat_until chains
  // that generate I/O cannot starve the kqueue filter set.
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
  // Bounded busy-spin: check for events and CPU tasks up to wait_spin_count
  // times before falling through to a blocking kevent().
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
  // Mark this worker sleeping so publishers can wake it.
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

  // A timeout only returns to the ready phase, which re-decides before the
  // worker sleeps again.
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
  // Runs while the worker is marked sleeping (only from wait_for_io_work()).
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
  // Closing forces finish regardless of inflight. A normal stop() waits for
  // every inflight I/O to produce its event, so finish() cannot race them.
  if (closing_requested()) {
    return true;
  }
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
  // Drain ready kevents, CPU tasks, and expired timers.
  for (;;) {
    (void)collect_ready_events(false);
    (void)consume_timeout_operations();
    if (!run_cpu_batch() && !consume_io_tasks()) break;
  }

  // Abort remaining inflight I/O. Only abnormal shutdown reaches here with
  // any: stop() waits for inflight_io_head_ to become null.
  abort_inflight_io();

  // Execute the abort completions. set_stopped() receivers may publish new
  // I/O on this thread into the local queue, so repeat until none remains.
  drain_local_cpu_tasks();
  while (consume_io_tasks()) {
    abort_inflight_io();
    drain_local_cpu_tasks();
  }

  run_state_.state.store(context_state::finished, std::memory_order_release);
}

}  // namespace bnio::async_io::bsd_native
