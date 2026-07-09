#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CLASS_SCOPE_
#error "This header is an io_uring_context class declaration fragment."
#endif

#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_RUN_LOOP_STATE_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_RUN_LOOP_STATE_H_

/**
 * Lifecycle state for the context run loop.
 */
enum class context_state {
  running,
  finishing,
  finished,
};

/**
 * Next action selected by the run loop.
 */
enum class run_phase {
  run_ready_tasks,
  wait_for_work,
  finish_drain,
  finished,
};

/**
 * Runs ready local and global tasks.
 */
[[nodiscard]] run_phase handle_run_ready_tasks(
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept;

/**
 * Waits for work when no tasks are immediately ready.
 */
[[nodiscard]] run_phase handle_wait_for_work(
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept;

/**
 * Drains remaining work during shutdown.
 */
[[nodiscard]] run_phase handle_finish_drain(
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept;

/**
 * Polls briefly for CQEs or posted tasks before blocking.
 */
[[nodiscard]] run_phase spin_for_work(operation_queue& local_tasks,
                                      unsigned& local_task_budget) noexcept;

/**
 * Waits on the condition variable while another thread waits for io_uring.
 */
[[nodiscard]] run_phase wait_for_condition_work(
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept;

/**
 * Waits for io_uring completion events.
 */
[[nodiscard]] run_phase wait_for_io_work(operation_queue& local_tasks,
                                         unsigned& local_task_budget) noexcept;

/**
 * Returns whether the context should leave the running state.
 */
[[nodiscard]] bool should_finish() const noexcept;

/**
 * Drains work and marks the context finished.
 */
void finish(operation_queue& local_tasks, unsigned& local_task_budget) noexcept;

unsigned wait_spin_count_ = io_uring_context_options{}.wait_spin_count;

static thread_local io_uring_context* current_context_;

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_RUN_LOOP_STATE_H_
