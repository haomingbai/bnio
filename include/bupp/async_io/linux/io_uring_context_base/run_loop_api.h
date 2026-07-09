#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CLASS_SCOPE_
#error "This header is an io_uring_context class declaration fragment."
#endif

#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_RUN_LOOP_API_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_RUN_LOOP_API_H_

/**
 * Runs queued tasks and CQE completions until the context finishes.
 */
void run() noexcept;

/**
 * Requests the run loop to finish.
 */
int stop() noexcept;

/**
 * Returns whether the current thread is running this context.
 */
[[nodiscard]] bool is_in_context() const noexcept;

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_RUN_LOOP_API_H_
