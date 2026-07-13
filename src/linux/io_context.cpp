#include <bupp/linux/io_context.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

namespace bupp {

using detail::native_worker;

thread_local detail::native_worker* io_context::current_native_worker_ =
    nullptr;

io_context::io_context() noexcept : io_context(io_context_options{}) {}

io_context::io_context(const io_context_options& options) noexcept
    : native_(options.platform, global_state_),
      timer_wakeup_operation_(*this),
      timer_update_operation_(*this),
      timer_driver_operation_(*this) {
  const std::size_t worker_count =
      std::max<std::size_t>(1, options.concurrency_hint);

  // Manually manage worker memory as an intrusive singly-linked list.
  native_workers_.head = new native_worker(*this);
  native_worker* current = native_workers_.head;
  for (std::size_t index = 1; index < worker_count; ++index) {
    auto* new_worker = new native_worker(*this);
    current->next.store(new_worker, std::memory_order_release);
    current = new_worker;
  }

  native_workers_.head->context.store(&native_.context,
                                      std::memory_order_release);
  native_workers_.round_robin_cursor.store(native_workers_.head,
                                           std::memory_order_release);
  native_workers_.active_count.store(native_.context.is_open() ? 1 : 0,
                                     std::memory_order_release);
}

io_context::~io_context() noexcept {
  std::lock_guard context_lock(timers_.mutex);
  for (auto& entry : timers_.timers) {
    detail::timer_slot* timer = entry.second;
    if (timer == nullptr) {
      continue;
    }
    std::lock_guard timer_lock(timer->mutex);
    timer->context = nullptr;
    timer->waiting_head = nullptr;
    timer->submitted_head.store(nullptr, std::memory_order_release);
  }
  timers_.timers.clear();
  timers_.heap.clear();

  // Manually delete all workers in the intrusive singly-linked list.
  native_worker* current = native_workers_.head;
  while (current != nullptr) {
    native_worker* next = current->next.load(std::memory_order_acquire);
    delete current;
    current = next;
  }
}

bool io_context::is_open() const noexcept { return native_.context.is_open(); }

void io_context::run() noexcept {
  native_worker* worker = register_run_worker();
  if (worker == nullptr) {
    return;
  }

  async_io::linux_native::io_uring_context* native_context =
      worker->context.load(std::memory_order_acquire);
  if (native_context == nullptr) {
    return;
  }

  native_worker* previous_worker = current_native_worker_;
  current_native_worker_ = worker;
  native_context->run();
  current_native_worker_ = previous_worker;
}

int io_context::stop() noexcept {
  global_state_.closing.store(true, std::memory_order_release);

  int first_error = 0;
  const std::size_t worker_count =
      native_workers_.active_count.load(std::memory_order_acquire);
  native_worker* worker = native_workers_.head;
  for (std::size_t index = 0; index < worker_count && worker != nullptr;
       ++index) {
    async_io::linux_native::io_uring_context* native_context =
        worker->context.load(std::memory_order_acquire);
    if (native_context != nullptr) {
      const int result = native_context->stop();
      if (result < 0 && first_error == 0) {
        first_error = result;
      }
    }
    worker = worker->next.load(std::memory_order_acquire);
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

detail::native_worker& io_context::select_worker() noexcept {
  native_worker* const head = native_workers_.head;

  native_worker* selected =
      native_workers_.round_robin_cursor.load(std::memory_order_acquire);
  if (selected == nullptr) {
    selected = head;
  }

  // Advance the round-robin cursor: move to next, wrap to head at end.
  native_worker* next = selected->next.load(std::memory_order_acquire);
  if (next == nullptr) {
    next = head;
  }
  native_workers_.round_robin_cursor.store(next, std::memory_order_release);

  // Return the selected worker if it has a valid open context.
  async_io::linux_native::io_uring_context* selected_context =
      selected == nullptr ? nullptr
                          : selected->context.load(std::memory_order_acquire);
  if (selected_context != nullptr && selected_context->is_open()) {
    return *selected;
  }

  // Fallback: walk the list to find a worker with a valid open context.
  native_worker* current = next;
  while (current != selected) {
    if (current == nullptr) {
      current = head;
      if (current == selected) break;
    }
    async_io::linux_native::io_uring_context* current_context =
        current->context.load(std::memory_order_acquire);
    if (current_context != nullptr && current_context->is_open()) {
      return *current;
    }
    current = current->next.load(std::memory_order_acquire);
  }

  return *head;
}

detail::native_worker& io_context::select_io_worker() noexcept {
  if (current_native_worker_ != nullptr &&
      current_native_worker_->owner == this) {
    async_io::linux_native::io_uring_context* native_context =
        current_native_worker_->context.load(std::memory_order_acquire);
    if (native_context != nullptr && native_context->is_open()) {
      return *current_native_worker_;
    }
  }
  return select_worker();
}

async_io::linux_native::io_uring_context&
io_context::select_native_context() noexcept {
  return *select_io_worker().context.load(std::memory_order_acquire);
}

detail::native_worker* io_context::register_run_worker() noexcept {
  if (global_state_.closing.load(std::memory_order_acquire)) {
    return nullptr;
  }

  const std::size_t index =
      native_workers_.next_run.fetch_add(1, std::memory_order_acq_rel);

  // Walk (or dynamically extend) the intrusive singly-linked list to reach
  // the worker at position `index`.
  native_worker* worker = native_workers_.head;
  for (std::size_t i = 0; i < index; ++i) {
    native_worker* next = worker->next.load(std::memory_order_acquire);
    if (next == nullptr) {
      // Dynamically extend the list with a new worker.
      auto* new_worker = new native_worker(*this);
      native_worker* expected = nullptr;
      if (worker->next.compare_exchange_strong(expected, new_worker,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
        next = new_worker;
      } else {
        // Another thread already appended a worker at this position — use
        // theirs and discard ours.
        delete new_worker;
        next = expected;
      }
    }
    worker = next;
  }

  if (index == 0) {
    async_io::linux_native::io_uring_context* native_context =
        worker->context.load(std::memory_order_acquire);
    if (native_context == nullptr || !native_context->is_open()) {
      return nullptr;
    }
  } else if (worker->context.load(std::memory_order_acquire) == nullptr) {
    worker->owned_context =
        std::make_unique<async_io::linux_native::io_uring_context>(
            native_.options.uring);
    if (!worker->owned_context->is_open()) {
      worker->owned_context.reset();
      return nullptr;
    }
    worker->owned_context->set_global_state(&global_state_);
    worker->context.store(worker->owned_context.get(),
                          std::memory_order_release);
  }

  const std::size_t published_count = index + 1;
  std::size_t current_count =
      native_workers_.active_count.load(std::memory_order_acquire);
  while (current_count < published_count &&
         !native_workers_.active_count.compare_exchange_weak(
             current_count, published_count, std::memory_order_acq_rel,
             std::memory_order_acquire)) {
  }

  // stop() may race with worker registration. If it observed the worker list
  // before this slot was published, do not let the late worker enter an idle
  // run loop that the completed stop scan could not have signalled.
  if (global_state_.closing.load(std::memory_order_acquire)) {
    async_io::linux_native::io_uring_context* native_context =
        worker->context.load(std::memory_order_acquire);
    if (native_context != nullptr) {
      (void)native_context->stop();
    }
    return nullptr;
  }

  return worker;
}

}  // namespace bupp
