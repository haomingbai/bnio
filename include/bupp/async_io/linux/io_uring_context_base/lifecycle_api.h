#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CLASS_SCOPE_
#error "This header is an io_uring_context class declaration fragment."
#endif

#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_LIFECYCLE_API_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_LIFECYCLE_API_H_

/**
 * Creates a closed context.
 */
io_uring_context() noexcept;

/**
 * Creates a context and attempts to initialize its ring.
 */
explicit io_uring_context(const io_uring_context_options& options) noexcept;

/**
 * Stops and releases the context ring.
 */
~io_uring_context() noexcept;

/**
 * Copy construction is disabled because the context owns an io_uring ring.
 */
io_uring_context(const io_uring_context&) = delete;

/**
 * Copy assignment is disabled because the context owns an io_uring ring.
 */
io_uring_context& operator=(const io_uring_context&) = delete;

/**
 * Move construction is disabled because the context owns synchronization
 * primitives and thread-local run-loop state.
 */
io_uring_context(io_uring_context&&) = delete;

/**
 * Move assignment is disabled because the context owns synchronization
 * primitives and thread-local run-loop state.
 */
io_uring_context& operator=(io_uring_context&&) = delete;

/**
 * Initializes the context ring with the supplied options.
 *
 * @see io_uring_queue_init
 */
int queue_init(const io_uring_context_options& options) noexcept;

/**
 * Releases the context ring and wakes any waiting run loop.
 *
 * @see io_uring_queue_exit
 */
void queue_exit() noexcept;

/**
 * Returns whether the context currently owns an open ring.
 */
[[nodiscard]] bool is_open() const noexcept;

/**
 * Returns the kernel io_uring feature flags reported at ring creation.
 *
 * Use IORING_FEAT_* macros to test individual capabilities, e.g.:
 *   if (ctx.kernel_features() & IORING_FEAT_RECVSEND_BUNDLE) { ... }
 *
 * @see io_uring_params::features
 */
[[nodiscard]] unsigned kernel_features() const noexcept {
  return kernel_features_;
}

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_LIFECYCLE_API_H_
