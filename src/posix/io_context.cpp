/**
 * @file io_context.cpp
 * @brief io_context construction, destruction, run loop entry.
 */

#include <bnio/detail/error_code.h>
#include <bnio/io_context.h>

#include <atomic>
#include <cerrno>
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
  // Shared wake channel: each worker registers read interest before
  // sleeping; one write wakes all of them, which is what stop() wants.
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

  // Close the channel under submit_lock after publishing the terminal state:
  // later submissions observe stopping, and no wake can race the close.
  {
    std::lock_guard guard(global_state_.submit_lock);
    global_state_.life_state.store(1, std::memory_order_release);
    global_state_.wake_channel_.close();
  }
}

bool io_context::is_open() const noexcept {
  // The backend is selected at compile time and construction always
  // succeeds; run() probes runtime availability and reports it via error_code.
  return true;
}

bool io_context::can_start_run() const noexcept {
  // A worker may enter the run loop only while the context is not stopping.
  return global_state_.life_state.load(std::memory_order_acquire) == 0;
}

void io_context::release_worker_slot() noexcept {
  global_state_.running_workers.fetch_sub(1, std::memory_order_acq_rel);
}
std::error_code io_context::run() noexcept {
  // Increment before any check so stop() cannot destroy the context while
  // run() is inside it; the counter lives in global_state_ for the backends.
  global_state_.running_workers.fetch_add(1, std::memory_order_acq_rel);

  if (!can_start_run()) {
    release_worker_slot();
    return std::make_error_code(std::errc::operation_canceled);
  }

  const std::error_code result = run_native_loop();
  release_worker_slot();
  return result;
}

std::error_code io_context::run_native_loop() noexcept {
  // Bind the native context before any check; its lifetime ends here, before
  // release_worker_slot(), so teardown never overlaps a zero worker count.
  std::error_code result = bnio::detail::empty_error_code;
  {
    detail::native_context ctx(native_options_);
    ctx.set_global_state(&global_state_);

    if (!ctx.is_open()) {
      result = std::error_code(ENOMEM, std::generic_category());
    } else if (global_state_.life_state.load(std::memory_order_acquire)) {
      // Close requested between the initial checks and the native open.
      (void)ctx.stop();
      result = std::make_error_code(std::errc::operation_canceled);
    } else {
      io_context* previous_context = current_context_;
      detail::native_context* previous_worker_native = current_worker_native_;
      current_context_ = this;
      current_worker_native_ = &ctx;
      ctx.run();
      // Restore both thread-local fast paths so a nested run() does not leave
      // them pointing at this destroyed context.
      current_worker_native_ = previous_worker_native;
      current_context_ = previous_context;
      // A failed native enter happened before any worker started; report
      // it instead of success for a loop that never ran.
      result = std::error_code(-ctx.enter_run_error(), std::generic_category());
    }
  }  // native context (and its queue_exit teardown) completes here

  return result;
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
  // Abort timer waits before publishing the stopping state, so a worker that
  // observes the stop and drains timers_.ready in finish() cannot miss them.
  abort_pending_timer_waits();
  // Publish stopping under the same lock as publish_cpu()'s state-check +
  // enqueue: operations that passed the check are ordered before the drain.
  {
    std::lock_guard<std::mutex> guard(global_state_.submit_lock);
    global_state_.life_state.store(1, std::memory_order_release);
  }
  return true;
}

int io_context::stop_internal() noexcept {
  // Wake unconditionally: a signal-handler caller excludes itself from the
  // wait (to avoid self-deadlock) but may still be blocked in the kernel; if it
  // is the last worker, this write is the only one that wakes its run loop.
  (void)global_state_.wake_channel_.wake();

  const bool in_worker_context = is_in_context();
  const std::size_t self_count = in_worker_context ? 1 : 0;
  while (global_state_.running_workers.load(std::memory_order_acquire) >
         self_count) {
    (void)global_state_.wake_channel_.wake();
    std::this_thread::yield();
  }

  // With running_workers at zero nothing else drains the shared queues;
  // deliver everything the stop was elected over, even if no worker started.
  drain_shared_queues_for_stop();

  return 0;
}

void io_context::drain_shared_queues_for_stop() noexcept {
  // Exclusive owner of the shared queues: racing publishes are visible here
  // or completed inline. Pop without submit_lock (callbacks may lock).
  bool had_work = true;
  while (had_work) {
    had_work = false;

    // Every queued operation must reach execute(): CPU work and staged timer
    // waits deliver operation_canceled, while I/O arbitrates on the receiver
    // stop token (set_stopped on a cancelled token, else operation_canceled);
    // no SQE or kevent is submitted.
    detail::native_operation_base* cpu = global_state_.pop_cpu_all();
    while (cpu != nullptr) {
      detail::native_operation_base* next = cpu->next;
      cpu->next = nullptr;
      had_work = true;
      cpu->execute();
      cpu = next;
    }

    if (global_state_.drain_io_stopped()) {
      had_work = true;
    }

    detail::timer_operation_base* ready = nullptr;
    {
      std::lock_guard context_lock(timers_.mutex);
      ready = timers_.ready;
      timers_.ready = nullptr;
    }
    while (ready != nullptr) {
      detail::timer_operation_base* next = ready->timer_next_;
      ready->timer_next_ = nullptr;
      had_work = true;
      ready->execute();
      ready = next;
    }
  }
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

io_context::defer_scheduler io_context::get_defer_scheduler() noexcept {
  return defer_scheduler(*this);
}

}  // namespace bnio
