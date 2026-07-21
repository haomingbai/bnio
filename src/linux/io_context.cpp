#include <bnio/linux/io_context.h>

#include <atomic>
#include <mutex>
#include <new>

#include "bnio/async_io/linux/io_uring_context_base/context.h"
#include "bnio/linux/detail/io_context_options.h"
#include "bnio/linux/detail/io_context_state.h"
#include "bnio/linux/detail/io_context_state/native_worker.h"
#include "bnio/linux/detail/io_context_timer_types.h"

namespace bnio {

using detail::native_worker;

thread_local detail::native_worker* io_context::current_native_worker_ =
    nullptr;

io_context::io_context() noexcept : io_context(io_context_options{}) {}

io_context::io_context(const io_context_options& options) noexcept
    : native_(options.platform) {
  {
    // Probe availability without reserving a worker or designating a primary
    // native context. Every actual ring is created by the thread that calls
    // run().
    async_io::linux_native::io_uring_context probe(native_.options.uring);
    native_available_.store(probe.is_open(), std::memory_order_release);
  }

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

  native_worker* current =
      native_workers_.head.exchange(nullptr, std::memory_order_acq_rel);
  while (current != nullptr) {
    native_worker* next = current->next;
    delete current;
    current = next;
  }
}

bool io_context::is_open() const noexcept {
  return native_available_.load(std::memory_order_acquire);
}

void io_context::run() noexcept {
  native_worker* worker = register_run_worker();
  if (worker == nullptr) {
    return;
  }

  native_worker* previous_worker = current_native_worker_;
  current_native_worker_ = worker;
  worker->context.run();
  current_native_worker_ = previous_worker;
}

int io_context::stop() noexcept {
  global_state_.closing.store(true, std::memory_order_release);

  int first_error = 0;
  native_worker* worker = native_workers_.head.load(std::memory_order_acquire);
  while (worker != nullptr) {
    const int result = worker->context.stop();
    if (result < 0 && first_error == 0) {
      first_error = result;
    }
    worker = worker->next;
  }
  return first_error;
}

bool io_context::is_in_context() const noexcept {
  return current_native_worker_ != nullptr &&
         current_native_worker_->owner == this;
}

io_context::dispatch_scheduler io_context::get_dispatch_scheduler() noexcept {
  return dispatch_scheduler(*this);
}

io_context::post_scheduler io_context::get_post_scheduler() noexcept {
  return post_scheduler(*this);
}

detail::native_worker* io_context::register_run_worker() noexcept {
  if (global_state_.closing.load(std::memory_order_acquire) ||
      !native_available_.load(std::memory_order_acquire)) {
    return nullptr;
  }

  auto* worker = new (std::nothrow) native_worker(*this, native_.options.uring);
  if (worker == nullptr || !worker->context.is_open()) {
    delete worker;
    return nullptr;
  }
  worker->context.set_global_state(global_state_);

  if (global_state_.closing.load(std::memory_order_acquire)) {
    (void)worker->context.stop();
    delete worker;
    return nullptr;
  }

  native_worker* head = native_workers_.head.load(std::memory_order_relaxed);
  do {
    worker->next = head;
  } while (!native_workers_.head.compare_exchange_weak(
      head, worker, std::memory_order_release, std::memory_order_relaxed));

  // A stop that raced with the head insertion may have completed its scan
  // before seeing this worker. Stop the local ring before declining to run.
  if (global_state_.closing.load(std::memory_order_acquire)) {
    (void)worker->context.stop();
    return nullptr;
  }

  return worker;
}

}  // namespace bnio
