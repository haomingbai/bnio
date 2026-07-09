#include <bupp/linux/io_context.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

namespace bupp {

thread_local io_context::native_worker* io_context::current_native_worker_ =
    nullptr;

io_context::io_context() noexcept : io_context(io_context_options{}) {}

io_context::io_context(const io_context_options& options) noexcept
    : native_context_(options.platform.uring),
      linux_options_(options.platform),
      timer_wakeup_operation_(*this),
      timer_update_operation_(*this),
      timer_driver_operation_(*this),
      queued_io_flush_timer_(*this) {
  const std::size_t worker_count =
      std::max<std::size_t>(1, options.concurrency_hint);
  native_workers_.reserve(worker_count);
  for (std::size_t index = 0; index < worker_count; ++index) {
    native_workers_.push_back(std::make_unique<native_worker>(*this));
  }
  native_workers_[0]->context = &native_context_;
  active_native_worker_count_.store(native_context_.is_open() ? 1 : 0,
                                    std::memory_order_release);
  timers_.queued_io_flush_wait.emplace(*this);
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
}

bool io_context::is_open() const noexcept { return native_context_.is_open(); }

void io_context::run() noexcept {
  native_worker* worker = register_run_worker();
  if (worker == nullptr || worker->context == nullptr) {
    return;
  }

  native_worker* previous_worker = current_native_worker_;
  current_native_worker_ = worker;
  worker->context->run();
  current_native_worker_ = previous_worker;
}

int io_context::stop() noexcept {
  stop_requested_.store(true, std::memory_order_release);

  int first_error = 0;
  const std::size_t worker_count =
      active_native_worker_count_.load(std::memory_order_acquire);
  for (std::size_t index = 0; index < worker_count; ++index) {
    native_worker* worker = native_workers_[index].get();
    if (worker == nullptr || worker->context == nullptr) {
      continue;
    }
    const int result = worker->context->stop();
    if (result < 0 && first_error == 0) {
      first_error = result;
    }
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

io_context::native_worker& io_context::primary_worker() noexcept {
  return *native_workers_[0];
}

io_context::native_worker& io_context::select_worker() noexcept {
  const std::size_t worker_count =
      active_native_worker_count_.load(std::memory_order_acquire);
  if (worker_count == 0) {
    return primary_worker();
  }

  for (std::size_t attempt = 0; attempt < worker_count; ++attempt) {
    const std::size_t index =
        next_native_worker_.fetch_add(1, std::memory_order_relaxed) %
        worker_count;
    native_worker* worker = native_workers_[index].get();
    if (worker != nullptr && worker->context != nullptr &&
        worker->context->is_open()) {
      return *worker;
    }
  }

  return primary_worker();
}

io_context::native_worker& io_context::select_io_worker() noexcept {
  if (current_native_worker_ != nullptr &&
      current_native_worker_->owner == this &&
      current_native_worker_->context != nullptr &&
      current_native_worker_->context->is_open()) {
    return *current_native_worker_;
  }
  return select_worker();
}

io_context::native_worker& io_context::ensure_operation_worker(
    operation_base& operation) noexcept {
  if (operation.native_worker_ == nullptr) {
    operation.native_worker_ = &select_io_worker();
  }
  return *operation.native_worker_;
}

async_io::linux_native::io_uring_context&
io_context::select_native_context() noexcept {
  return *select_io_worker().context;
}

io_context::native_worker* io_context::register_run_worker() noexcept {
  if (stop_requested_.load(std::memory_order_acquire)) {
    return nullptr;
  }

  const std::size_t index =
      next_run_worker_.fetch_add(1, std::memory_order_acq_rel);
  if (index >= native_workers_.size()) {
    return nullptr;
  }

  native_worker& worker = *native_workers_[index];
  if (index == 0) {
    if (worker.context == nullptr || !worker.context->is_open()) {
      return nullptr;
    }
  } else if (worker.context == nullptr) {
    worker.owned_context =
        std::make_unique<async_io::linux_native::io_uring_context>(
            linux_options_.uring);
    if (!worker.owned_context->is_open()) {
      worker.owned_context.reset();
      return nullptr;
    }
    worker.context = worker.owned_context.get();
  }

  const std::size_t published_count = index + 1;
  std::size_t current_count =
      active_native_worker_count_.load(std::memory_order_acquire);
  while (current_count < published_count &&
         !active_native_worker_count_.compare_exchange_weak(
             current_count, published_count, std::memory_order_acq_rel,
             std::memory_order_acquire)) {
  }

  return &worker;
}

}  // namespace bupp
