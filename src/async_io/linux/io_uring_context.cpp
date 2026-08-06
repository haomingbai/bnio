/**
 * @file io_uring_context.cpp
 * @brief io_uring_context lifecycle, queue initialization, and fallback logic.
 */

#include <bnio/async_io/linux/io_uring_context.h>
#include <bnio/base/linux/liburing.h>
#include <bnio/base/linux/params.h>

#include <atomic>
#include <cerrno>

#include "io_uring_context_internal.h"

namespace bnio::async_io::linux_native {

namespace {

unsigned prepare_queue_params(const io_uring_context_options& options,
                              bnio::base::params& queue_params) noexcept {
  unsigned flags = options.setup_flags;
  if ((flags & bnio::base::detail::io_uring_setup_single_issuer) != 0) {
    flags |= bnio::base::detail::io_uring_setup_r_disabled;
  }
  if (options.enable_sqpoll) {
    flags |= IORING_SETUP_SQPOLL;
    queue_params.set_sq_thread_cpu(options.sqpoll_thread_cpu);
    queue_params.set_sq_thread_idle(options.sqpoll_idle_ms);
  }
  return flags;
}

}  // namespace

thread_local io_uring_context* io_uring_context::current_context_ = nullptr;

io_uring_context::io_uring_context() noexcept = default;

io_uring_context::io_uring_context(
    const io_uring_context_options& options) noexcept
    : io_uring_context() {
  (void)queue_init(options);
}

io_uring_context::~io_uring_context() noexcept { queue_exit(); }

void io_uring_context::apply_context_options(
    const io_uring_context_options& options) noexcept {
  options_ = options;
  if (options_.cqe_batch_window == 0) {
    options_.cqe_batch_window = 1;
  }
}

int io_uring_context::queue_init(
    const io_uring_context_options& options) noexcept {
  if (run_state_.queue_initialized) {
    return -EALREADY;
  }
  run_state_.run_active.store(false, std::memory_order_release);
  run_state_.queue_initialized = true;
  apply_context_options(options);
  poll_state_.eventfd_poll_pending = false;
  poll_state_.local_eventfd_poll_pending = false;
  (void)local_state_.wake_channel_.open();

  bnio::base::params queue_params;
  const unsigned flags = prepare_queue_params(options_, queue_params);
  const int result = init_ring_params(options_.entries, flags, queue_params);

  if (result >= 0) {
    kernel_features_ = queue_params.features();
    run_state_.ring_disabled =
        (queue_params.flags() &
         bnio::base::detail::io_uring_setup_r_disabled) != 0;
  } else {
    kernel_features_ = 0;
    run_state_.ring_disabled = false;
  }

  if (result < 0) {
    run_state_.state.store(context_state::finished, std::memory_order_release);
    return result;
  }

  run_state_.state.store(context_state::running, std::memory_order_release);
  return 0;
}

int io_uring_context::init_ring_params(
    unsigned entries, unsigned flags,
    bnio::base::params& queue_params) noexcept {
  queue_params.set_flags(flags);

  int result = ring_.queue_init_params(entries, queue_params);
  if (result >= 0) {
    return result;
  }

  // Fallback: retry without optional task-run/issuer flags on older kernels.
  // Explicit SQPOLL requests are retained and permission/resource failures are
  // propagated instead of being silently converted to a different mode.
  if (result == -EINVAL && flags != 0) {
    queue_params.reset();
    flags &= ~(bnio::base::detail::io_uring_setup_coop_taskrun |
               bnio::base::detail::io_uring_setup_single_issuer |
               bnio::base::detail::io_uring_setup_r_disabled);
    queue_params.set_flags(flags);
    if ((flags & IORING_SETUP_SQPOLL) != 0) {
      queue_params.set_sq_thread_cpu(options_.sqpoll_thread_cpu);
      queue_params.set_sq_thread_idle(options_.sqpoll_idle_ms);
    }
    result = ring_.queue_init_params(entries, queue_params);
  }

  return result;
}

void io_uring_context::queue_exit() noexcept {
  // Abort any remaining inflight I/O before closing the ring.
  // Normal shutdown cleans these up in finish(), but forced/abnormal
  // shutdown (e.g. closing flag, destruction without run) may leave
  // operations in-flight.
  abort_inflight_io();

  run_state_.state.store(context_state::finished, std::memory_order_release);

  (void)local_state_.pop_cpu_all();
  poll_state_.eventfd_poll_pending = false;
  poll_state_.local_eventfd_poll_pending = false;
  local_state_.wake_channel_.close();
  run_state_.ring_disabled = false;
  ring_.queue_exit();
  run_state_.queue_initialized = false;
}

bool io_uring_context::is_open() const noexcept { return ring_.is_open(); }

void io_uring_context::set_global_state(
    io_uring_task_queue_state* state) noexcept {
  global_state_ = state;
}

}  // namespace bnio::async_io::linux_native
