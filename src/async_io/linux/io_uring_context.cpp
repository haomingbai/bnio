#include <bupp/async_io/linux/io_uring_context.h>
#include <bupp/base/linux/liburing.h>
#include <bupp/base/linux/params.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cerrno>

#include "io_uring_context_internal.h"

namespace bupp::async_io::linux_native {

namespace {

unsigned prepare_queue_params(const io_uring_context_options& options,
                              bupp::base::params& queue_params) noexcept {
  unsigned flags = options.setup_flags;
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
  if (queue_initialized_) {
    return -EALREADY;
  }
  run_active_.store(false, std::memory_order_release);
  queue_initialized_ = true;
  apply_context_options(options);
  eventfd_poll_pending_ = false;

  if (options_.event_fd >= 0) {
    event_fd_ = options_.event_fd;
    owns_event_fd_ = false;
  } else {
    event_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (event_fd_ < 0) {
      const int error = -errno;
      queue_initialized_ = false;
      return error;
    }
    owns_event_fd_ = true;
  }

  bupp::base::params queue_params;
  const unsigned flags = prepare_queue_params(options_, queue_params);
  const int result = init_ring_params(options_.entries, flags, queue_params);

  if (result >= 0) {
    kernel_features_ = queue_params.features();
  } else {
    kernel_features_ = 0;
  }

  if (result < 0) {
    state_.store(context_state::finished, std::memory_order_release);
    if (owns_event_fd_ && event_fd_ >= 0) {
      (void)::close(event_fd_);
    }
    event_fd_ = -1;
    owns_event_fd_ = false;
    return result;
  }

  state_.store(context_state::running, std::memory_order_release);
  return 0;
}

int io_uring_context::init_ring_params(
    unsigned entries, unsigned flags,
    bupp::base::params& queue_params) noexcept {
  queue_params.set_flags(flags);

  int result = ring_.queue_init_params(entries, queue_params);
  if (result >= 0) {
    return result;
  }

  // Fallback: retry without bupp-managed setup flags for older kernels.
  if (flags != 0) {
    queue_params.reset();
    flags &= ~(bupp::base::detail::io_uring_setup_coop_taskrun |
               IORING_SETUP_SQPOLL);
    queue_params.set_flags(flags);
    result = ring_.queue_init_params(entries, queue_params);
  }

  return result;
}

void io_uring_context::queue_exit() noexcept {
  state_.store(context_state::finished, std::memory_order_release);
  (void)signal_eventfd();

  (void)local_tasks_.pop_all();
  local_io_tasks_ = nullptr;
  eventfd_poll_pending_ = false;
  ring_.queue_exit();
  if (owns_event_fd_ && event_fd_ >= 0) {
    (void)::close(event_fd_);
  }
  event_fd_ = -1;
  owns_event_fd_ = false;
  queue_initialized_ = false;
}

bool io_uring_context::is_open() const noexcept { return ring_.is_open(); }

void io_uring_context::set_global_state(
    io_uring_task_queue_state* state) noexcept {
  assert(!run_active_.load(std::memory_order_acquire));
  global_state_ = state;
}

}  // namespace bupp::async_io::linux_native
