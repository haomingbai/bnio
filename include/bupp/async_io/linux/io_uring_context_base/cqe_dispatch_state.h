#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CLASS_SCOPE_
#error "This header is an io_uring_context class declaration fragment."
#endif

#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CQE_DISPATCH_STATE_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CQE_DISPATCH_STATE_H_

/**
 * Small copy of CQE data used after the CQ head advances.
 */
struct cqe_data;

/**
 * Waits for at least one CQE event on the native ring descriptor.
 */
[[nodiscard]] int wait_for_cqe_event() noexcept;

/**
 * Collects and dispatches ready CQE-backed tasks.
 */
[[nodiscard]] bool collect_ready_cqes(operation_queue& local_tasks,
                                      unsigned& local_task_budget,
                                      bool wait_for_gate = false) noexcept;

/**
 * Collects ready CQEs into an operation queue.
 */
[[nodiscard]] unsigned collect_cqe_tasks(operation_queue& cqe_tasks,
                                         bool wait_for_gate) noexcept;

/**
 * Dispatches collected CQE tasks locally or through the global queue.
 */
void dispatch_cqe_tasks(operation_queue& cqe_tasks, unsigned task_count,
                        operation_queue& local_tasks,
                        unsigned& local_task_budget) noexcept;

/**
 * Enqueues the operation represented by one CQE data record.
 */
[[nodiscard]] bool enqueue_cqe_task(const cqe_data& data,
                                    operation_queue& tasks) noexcept;

unsigned cqe_batch_window_ = io_uring_context_options{}.cqe_batch_window;
unsigned cqe_inline_completion_threshold_ =
    io_uring_context_options{}.cqe_inline_completion_threshold;
unsigned local_queue_threshold_ =
    io_uring_context_options{}.local_queue_threshold;

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CQE_DISPATCH_STATE_H_
