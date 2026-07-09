#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CLASS_SCOPE_
#error "This header is an io_uring_context class declaration fragment."
#endif

#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_TASK_QUEUE_STATE_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_TASK_QUEUE_STATE_H_

// Per-run-scope task stack. This is intentionally not atomic: only the
// owning run() thread pushes into it and drains it with pop_all. Cross-thread
// producers must publish through global_tasks_ instead.
/**
 * Intrusive stack of operations used by run-loop phases.
 */
struct operation_queue;

/**
 * Publishes a single operation to the global task queue.
 */
void push_global_task(io_uring_operation_base& operation) noexcept;

/**
 * Publishes all operations from a local queue to the global task queue.
 */
void push_global_tasks(operation_queue& operations) noexcept;

/**
 * Moves globally published tasks into a local queue.
 */
[[nodiscard]] bool move_global_tasks(operation_queue& local_tasks) noexcept;

/**
 * Submits a wake-up SQE for an active io_uring waiter.
 */
int submit_wake_task() noexcept;

/**
 * Submits a wake-up SQE while the uring mutex is already held.
 */
int submit_wake_task_locked() noexcept;

/**
 * Returns the sentinel user data used for wake-up SQEs.
 */
[[nodiscard]] static void* wake_user_data() noexcept;

// Cross-thread task publication stack. run() workers drain it in one batch
// and reverse the batch before moving work into their local operation_queue.
std::atomic<io_uring_operation_base*> global_tasks_{nullptr};
std::mutex wait_mutex_;
std::condition_variable wait_cv_;
std::atomic_bool io_waiter_active_{false};
bool wake_task_pending_ = false;

static thread_local operation_queue* current_local_tasks_;

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_TASK_QUEUE_STATE_H_
