/**
 * @file io_context.cpp
 * @brief io_context construction, destruction, run loop entry.
 */

#include <bnio/io_context.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace bnio {

thread_local io_context* io_context::current_context_ = nullptr;
thread_local detail::native_context* io_context::current_worker_native_ =
    nullptr;

io_context::io_context() noexcept : io_context(io_context_options{}) {}

io_context::io_context(const io_context_options& options) noexcept
    : native_(options.platform) {
  {
    // Probe availability without reserving a worker or designating a primary
    // native context. Every actual native queue is created by the thread that
    // calls run().
    detail::native_context probe(native_.options);
    native_available_.store(probe.is_open(), std::memory_order_release);
  }

  // Create the shared wake channel owned by io_context. Each worker's
  // native context registers read interest on the channel before
  // sleeping. io_context writes to the channel to wake workers.
  //
  // @warning A single write wakes ALL workers that have read interest
  // registered on the channel (minor thundering herd). The per-worker
  // overhead is one read(2) returning EAGAIN plus one pop_cpu_all()
  // CAS — negligible for typical 4–8 worker concurrency. During
  // stop(), waking all workers is the desired behaviour.
  (void)global_state_.wake_channel_.open();

  global_state_.timeout_heap = &timers_;
  global_state_.try_fetch_timeout_operations =
      &io_context::try_fetch_timeout_operations_thunk;
  timers_.owner = this;
}

io_context::~io_context() noexcept {
  {
    std::lock_guard context_lock(timers_.mutex);
    timers_.clear();
    timers_.owner = nullptr;
    global_state_.timeout_heap = nullptr;
    global_state_.try_fetch_timeout_operations = nullptr;
  }

  // The wake channel is owned by io_context and outlives all workers.
  global_state_.wake_channel_.close();
}

bool io_context::is_open() const noexcept {
  return native_available_.load(std::memory_order_acquire);
}

void io_context::run() noexcept {
  // Increment running_workers BEFORE any check so stop() always observes
  // this worker regardless of how far run() has progressed.  This closes
  // the use-after-free window: if stop() destroys the io_context while a
  // worker is inside run(), the worker would access freed memory.
  // Previously each native backend performed this increment deep inside
  // its own run() loop, which left a window between set_global_state()
  // and the increment where stop() saw 0 workers.
  running_workers_.fetch_add(1, std::memory_order_acq_rel);

  if (global_state_.life_state.load(std::memory_order_acquire) != 0 ||
      !native_available_.load(std::memory_order_acquire)) {
    running_workers_.fetch_sub(1, std::memory_order_acq_rel);
    return;
  }

  detail::native_context ctx(native_.options);
  if (!ctx.is_open()) {
    running_workers_.fetch_sub(1, std::memory_order_acq_rel);
    return;
  }
  ctx.set_global_state(&global_state_);

  // A close may have been requested after the initial checks but before
  // the native context was fully opened.
  if (global_state_.life_state.load(std::memory_order_acquire)) {
    (void)ctx.stop();
    ctx.set_global_state(nullptr);
    running_workers_.fetch_sub(1, std::memory_order_acq_rel);
    return;
  }

  io_context* previous_context = current_context_;
  current_context_ = this;
  current_worker_native_ = &ctx;
  ctx.run();
  // Clear the pointer so the native context destructor does not
  // access global_state_ after stop() returns and the caller
  // destroys the io_context.
  ctx.set_global_state(nullptr);
  running_workers_.fetch_sub(1, std::memory_order_acq_rel);
  current_worker_native_ = nullptr;
  current_context_ = previous_context;
}

int io_context::stop() noexcept {
  // Try to become the stopping thread.
  int expected = 0;  // running
  if (!global_state_.life_state.compare_exchange_strong(
          expected, 1,  // 1 = stopping
          std::memory_order_acq_rel, std::memory_order_acquire)) {
    // Already stopping or stopped — spin until fully stopped.
    while (!stopped_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    return 0;
  }

  // We own the stop. Perform the actual stop work and signal completion.
  stop_internal();
  stopped_.store(true, std::memory_order_release);
  return 0;
}

int io_context::stop_internal() noexcept {
  // Abort all pending timer waits BEFORE workers observe the stopping
  // state (already set by caller).  This keeps set_stopped as the sole
  // signal for timer operations during shutdown.
  abort_pending_timer_waits();

  // Repeatedly signal the shared wake channel until every *other*
  // worker has observed the stopping flag and exited.
  const bool in_worker_context = is_in_context();
  const std::size_t self_count = in_worker_context ? 1 : 0;
  while (running_workers_.load(std::memory_order_acquire) >
         self_count) {
    (void)global_state_.wake_channel_.wake();
    std::this_thread::yield();
  }

  return 0;
}

io_context::join_sender io_context::join() noexcept {
  return join_sender(*this);
}

bool io_context::is_in_context() const noexcept {
  return current_context_ == this;
}

io_context::dispatch_scheduler io_context::get_dispatch_scheduler() noexcept {
  return dispatch_scheduler(*this);
}

io_context::post_scheduler io_context::get_post_scheduler() noexcept {
  return post_scheduler(*this);
}

}  // namespace bnio
