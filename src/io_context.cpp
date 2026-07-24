/**
 * @file io_context.cpp
 * @brief io_context construction, destruction, run loop entry, and worker
 * registration.
 */

#include <bnio/io_context.h>

#include <atomic>
#include <mutex>

#include "bnio/detail/io_context/native_worker.h"

namespace bnio {

using detail::native_worker;

thread_local detail::native_worker* io_context::current_native_worker_ =
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

  // Workers are stack-allocated inside run(); just clear the list head.
  native_workers_.head.store(nullptr, std::memory_order_release);
}

bool io_context::is_open() const noexcept {
  return native_available_.load(std::memory_order_acquire);
}

void io_context::run() noexcept {
  if (global_state_.closing.load(std::memory_order_acquire) ||
      !native_available_.load(std::memory_order_acquire)) {
    return;
  }

  native_worker worker{*this, native_.options};
  if (!prepare_run_worker(worker)) {
    return;
  }

  native_worker* previous_worker = current_native_worker_;
  current_native_worker_ = &worker;
  worker.context.run();
  current_native_worker_ = previous_worker;
}

bool io_context::prepare_run_worker(
    detail::native_worker& worker) noexcept {
  if (!worker.context.is_open()) {
    return false;
  }
  worker.context.set_global_state(&global_state_);

  // A close may have been requested after the initial checks but before
  // the native context was fully opened.
  if (global_state_.closing.load(std::memory_order_acquire)) {
    (void)worker.context.stop();
    return false;
  }

  // CAS-insert this worker at the head of the lock-free worker list.
  native_worker* head = native_workers_.head.load(std::memory_order_relaxed);
  do {
    worker.next = head;
  } while (!native_workers_.head.compare_exchange_weak(
      head, &worker, std::memory_order_release, std::memory_order_relaxed));

  // A stop that raced with the head insertion may have completed its scan
  // before seeing this worker.
  if (global_state_.closing.load(std::memory_order_acquire)) {
    (void)worker.context.stop();
    return false;
  }

  return true;
}

int io_context::stop() noexcept {
  global_state_.closing.store(true, std::memory_order_release);

  int first_error = 0;
  native_worker* worker = native_workers_.head.load(std::memory_order_acquire);
  while (worker != nullptr) {
    native_worker* next = worker->next;
    const int result = worker->context.stop();
    if (result < 0 && first_error == 0) {
      first_error = result;
    }
    worker = next;
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

}  // namespace bnio
