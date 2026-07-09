#include <bupp/linux/io_context.h>

#include <atomic>
#include <mutex>

namespace bupp {

io_context::io_context() noexcept : io_context(io_context_options{}) {}

io_context::io_context(const io_context_options& options) noexcept
    : native_context_(options.platform.uring),
      linux_options_(options.platform),
      timer_wakeup_operation_(*this),
      timer_update_operation_(*this),
      timer_driver_operation_(*this),
      queued_io_flush_timer_(*this) {
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

void io_context::run() noexcept { native_context_.run(); }

int io_context::stop() noexcept { return native_context_.stop(); }

bool io_context::is_in_context() const noexcept {
  return native_context_.is_in_context();
}

io_context::dispatch_scheduler io_context::get_dispatch_scheduler() noexcept {
  return dispatch_scheduler(*this);
}

io_context::post_scheduler io_context::get_post_scheduler() noexcept {
  return post_scheduler(*this);
}

}  // namespace bupp
