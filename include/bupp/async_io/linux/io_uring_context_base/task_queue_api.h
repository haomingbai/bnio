#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CLASS_SCOPE_
#error "This header is an io_uring_context class declaration fragment."
#endif

#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_TASK_QUEUE_API_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_TASK_QUEUE_API_H_

/**
 * Posts an operation for execution by the context run loop.
 */
int post(io_uring_operation_base& operation) noexcept;

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_TASK_QUEUE_API_H_
