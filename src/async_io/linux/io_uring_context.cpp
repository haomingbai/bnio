#include <bupp/async_io/linux/io_uring_context.h>
#include <bupp/base/linux/liburing.h>
#include <bupp/base/linux/params.h>

#include <atomic>
#include <cerrno>

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
thread_local io_uring_context::operation_queue*
    io_uring_context::current_local_tasks_ = nullptr;

io_uring_context::io_uring_context() noexcept = default;

io_uring_context::io_uring_context(
    const io_uring_context_options& options) noexcept {
  (void)queue_init(options);
}

io_uring_context::~io_uring_context() noexcept { queue_exit(); }

void io_uring_context::apply_context_options(
    const io_uring_context_options& options) noexcept {
  cqe_batch_window_ =
      options.cqe_batch_window == 0 ? 1 : options.cqe_batch_window;
  wait_spin_count_ = options.wait_spin_count;
  cqe_inline_completion_threshold_ = options.cqe_inline_completion_threshold;
  local_queue_threshold_ = options.local_queue_threshold;
}

int io_uring_context::queue_init(
    const io_uring_context_options& options) noexcept {
  global_tasks_.store(nullptr, std::memory_order_release);
  io_waiter_active_.store(false, std::memory_order_release);

  if (queue_initialized_) {
    return -EALREADY;
  }
  queue_initialized_ = true;
  apply_context_options(options);
  wake_task_pending_ = false;

  bupp::base::params queue_params;
  const unsigned flags = prepare_queue_params(options, queue_params);
  const int result = init_ring_params(options.entries, flags, queue_params);

  if (result >= 0) {
    kernel_features_ = queue_params.features();
  } else {
    kernel_features_ = 0;
  }

  state_.store(result >= 0 ? context_state::running : context_state::finished,
               std::memory_order_release);
  return result;
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
  io_waiter_active_.store(false, std::memory_order_release);
  global_tasks_.store(nullptr, std::memory_order_release);
  notify_waiters();

  wake_task_pending_ = false;
  ring_.queue_exit();
}

bool io_uring_context::is_open() const noexcept { return ring_.is_open(); }

}  // namespace bupp::async_io::linux_native
