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
    : native_options_(options.platform),
      immutable_flags_(options.enable_immediate_io) {
  {
    // Availability is checked lazily by run(), which creates the actual
    // native context on the worker thread.  Each run() call serves as
    // its own probe and returns an error_code when the backend is
    // unavailable.
    immutable_flags_.native_available.store(true, std::memory_order_release);
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

  // Bind the wake-channel close to the submit lock. publish_cpu /
  // publish_io and every wake helper write the channel inside the same
  // lock, so a close can never race a wake write (Issue 2's write-after-
  // close use-after-free). Publishing the terminal state first also makes
  // every later submission observe the stopping state and reject inline
  // instead of touching the queue or the channel after destruction begins.
  {
    std::lock_guard guard(global_state_.submit_lock);
    global_state_.life_state.store(1, std::memory_order_release);
    global_state_.wake_channel_.close();
  }
}

bool io_context::is_open() const noexcept {
  return immutable_flags_.native_available.load(std::memory_order_acquire);
}

bool io_context::can_start_run() const noexcept {
  // A worker may enter the run loop only while the context is not stopping
  // and the native backend was available at construction.
  return global_state_.life_state.load(std::memory_order_acquire) == 0 &&
         immutable_flags_.native_available.load(std::memory_order_acquire);
}

void io_context::release_worker_slot() noexcept {
  global_state_.running_workers.fetch_sub(1, std::memory_order_acq_rel);
}
std::error_code io_context::run() noexcept {
  // Increment running_workers BEFORE any check so stop() always observes
  // this worker regardless of how far run() has progressed.  This closes
  // the use-after-free window: if stop() destroys the io_context while a
  // worker is inside run(), the worker would access freed memory.
  // The counter lives in global_state_ so the native backends' advisory
  // heuristics (steal gate, wake fast path) share the same count; the
  // increment itself stays here, before any check, never inside
  // enter_run().
  global_state_.running_workers.fetch_add(1, std::memory_order_acq_rel);

  if (!can_start_run()) {
    release_worker_slot();
    return std::make_error_code(std::errc::operation_canceled);
  }

  detail::native_context ctx(native_options_);
  if (!ctx.is_open()) {
    release_worker_slot();
    return std::error_code(ENOMEM, std::generic_category());
  }
  ctx.set_global_state(&global_state_);

  // A close may have been requested after the initial checks but before
  // the native context was fully opened.
  if (global_state_.life_state.load(std::memory_order_acquire)) {
    (void)ctx.stop();
    ctx.set_global_state(nullptr);
    release_worker_slot();
    return std::make_error_code(std::errc::operation_canceled);
  }

  io_context* previous_context = current_context_;
  current_context_ = this;
  current_worker_native_ = &ctx;
  ctx.run();
  // Clear the pointer so the native context destructor does not
  // access global_state_ after stop() returns and the caller
  // destroys the io_context.
  ctx.set_global_state(nullptr);
  release_worker_slot();
  current_worker_native_ = nullptr;
  current_context_ = previous_context;
  return std::error_code{};
}

int io_context::stop() noexcept {
  if (!begin_stop()) {
    // Already stopping or stopped — spin until fully stopped.
    while (!lifecycle_.stopped.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    return 0;
  }

  // We own the stop. Perform the actual stop work and signal completion.
  stop_internal();
  lifecycle_.stopped.store(true, std::memory_order_release);
  return 0;
}

bool io_context::begin_stop() noexcept {
  // Elect exactly one stopping thread; stop() and join() share this path.
  int expected = 0;
  if (!lifecycle_.stop_requested.compare_exchange_strong(
          expected, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
    return false;
  }
  // Abort all pending timer waits BEFORE publishing the stopping state.
  // This guarantees that when workers observe life_state != 0 and enter
  // finish(), the aborted operations are already staged on timers_.ready.
  // Without this ordering a worker that is actively spinning (not sleeping
  // in a syscall) could observe life_state == 1, drain the still-empty
  // timers_.ready in finish(), and exit before abort_pending_timer_waits()
  // moves the operations — leaving them permanently stranded.
  abort_pending_timer_waits();
  // Publish the stopping state inside the same lock used by publish_cpu()'s
  // state-check + enqueue critical section, binding enqueue to the state.
  // Any operation that passed the check before this store is ordered before
  // the workers' final drain and is guaranteed to complete.
  {
    std::lock_guard<std::mutex> guard(global_state_.submit_lock);
    global_state_.life_state.store(1, std::memory_order_release);
  }
  return true;
}

int io_context::stop_internal() noexcept {
  // Timer waits were already aborted by begin_stop() before the stopping
  // state was published.  Now wait for every other worker to observe the
  // stopping flag and exit.  Workers drain timers_.ready during finish(),
  // and the ordering in begin_stop() guarantees the operations are already
  // staged there before any worker can enter its final drain.
  const bool in_worker_context = is_in_context();
  const std::size_t self_count = in_worker_context ? 1 : 0;
  while (global_state_.running_workers.load(std::memory_order_acquire) >
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
