#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_CLASS_SCOPE_
#error "This header is an io_uring_context class declaration fragment."
#endif

#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_LIFECYCLE_STATE_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_LIFECYCLE_STATE_H_

/**
 * Initialises the ring with the supplied flags, retrying without bupp-managed
 * setup flags on EINVAL.
 *
 * @return 0 on success, or a negative errno.
 */
int init_ring_params(unsigned entries, unsigned flags,
                     bupp::base::params& queue_params) noexcept;

/**
 * Applies configuration from options to context member variables.
 */
void apply_context_options(const io_uring_context_options& options) noexcept;

/**
 * Verifies in debug builds that the context is running.
 */
void assert_running() const noexcept;

bupp::base::ring ring_;
mutable std::mutex uring_mutex_;
unsigned kernel_features_ = 0;
std::atomic<context_state> state_{context_state::finished};
bool queue_initialized_ = false;

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_LIFECYCLE_STATE_H_
