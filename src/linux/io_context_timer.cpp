#include <bnio/linux/io_context.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "bnio/async_io/linux/io_uring_operations/helpers.h"
#include "bnio/async_io/time.h"
#include "bnio/linux/detail/io_context_timer_types.h"
#include "bnio/linux/detail/steady_timer.h"

namespace bnio {

namespace detail {

timer_operation_base::timer_operation_base(io_context& context) noexcept
    : timer_context_(&context) {}

}  // namespace detail

detail::timer_wakeup_operation::timer_wakeup_operation(
    io_context& context) noexcept
    : context_(&context) {}

void detail::timer_wakeup_operation::set_timeout(
    async_io::duration timeout) noexcept {
  timeout_.reset(timeout);
}

void detail::timer_wakeup_operation::prepare(
    base::submission_queue_entry& sqe) noexcept {
  timeout_.prepare_timeout(sqe, 0, 0);
}

void detail::timer_wakeup_operation::complete_submit_error(
    int submit_result) noexcept {
  result = submit_result;
}

void detail::timer_wakeup_operation::execute() noexcept {
  context_->on_timer_wakeup();
}

detail::timer_update_operation::timer_update_operation(
    io_context& context) noexcept
    : context_(&context) {}

void detail::timer_update_operation::set_timeout(
    async_io::duration timeout) noexcept {
  timeout_.reset(timeout);
}

void detail::timer_update_operation::prepare(
    base::submission_queue_entry& sqe) noexcept {
  timeout_.prepare_timeout_update(
      sqe,
      static_cast<std::uint64_t>(
          reinterpret_cast<std::uintptr_t>(&context_->timer_wakeup_operation_)),
      0);
}

void detail::timer_update_operation::complete_submit_error(
    int submit_result) noexcept {
  result = submit_result;
}

void detail::timer_update_operation::execute() noexcept {
  context_->on_timer_update();
}

detail::timer_driver_operation::timer_driver_operation(
    io_context& context) noexcept
    : context_(&context) {}

void detail::timer_driver_operation::execute() noexcept {
  context_->on_timer_driver();
}

void io_context::register_timer(detail::timer_slot& timer) noexcept {
  std::lock_guard context_lock(timers_.mutex);
  std::lock_guard timer_lock(timer.mutex);
  if (timer.context != nullptr) {
    return;
  }

  timer.id = timers_.next_timer_id++;
  if (timer.id == 0) {
    timer.id = timers_.next_timer_id++;
  }
  timer.context = this;
  timer.generation = 0;
  timer.waiting_head = nullptr;
  timer.submitted_head.store(nullptr, std::memory_order_release);
  timers_.timers.emplace(timer.id, &timer);
}

void io_context::unregister_timer(detail::timer_slot& timer) noexcept {
  detail::submitted_timer_operations canceled;
  {
    std::lock_guard context_lock(timers_.mutex);
    std::lock_guard timer_lock(timer.mutex);
    if (timer.context != this) {
      return;
    }

    auto iterator = timers_.timers.find(timer.id);
    if (iterator != timers_.timers.end()) {
      detail::timer_slot* mapped_timer = iterator->second;
      if (mapped_timer != nullptr && mapped_timer == &timer) {
        timers_.timers.erase(iterator);
      }
    }

    ++timer.generation;
    timer.context = nullptr;
    canceled = take_all_submitted_timer_operations(timer);
  }

  post_submitted_timer_operations(canceled,
                                  detail::timer_completion_kind::stopped);
}

std::size_t io_context::cancel_timer(detail::timer_slot& timer) noexcept {
  detail::submitted_timer_operations canceled;
  std::size_t count = 0;
  {
    std::lock_guard context_lock(timers_.mutex);
    std::lock_guard timer_lock(timer.mutex);
    if (timer.context != this) {
      return 0;
    }

    ++timer.generation;
    canceled = take_all_submitted_timer_operations(timer);
  }

  count = count_submitted_timer_operations(canceled);
  post_submitted_timer_operations(canceled,
                                  detail::timer_completion_kind::stopped);
  post_timer_driver();
  return count;
}

std::size_t io_context::set_timer_expiry(detail::timer_slot& timer,
                                         time_point expiry) noexcept {
  detail::submitted_timer_operations canceled;
  std::size_t count = 0;
  {
    std::lock_guard context_lock(timers_.mutex);
    std::lock_guard timer_lock(timer.mutex);
    if (timer.context != this) {
      return 0;
    }

    timer.expiry = expiry;
    ++timer.generation;
    canceled = take_all_submitted_timer_operations(timer);
  }

  count = count_submitted_timer_operations(canceled);
  post_submitted_timer_operations(canceled,
                                  detail::timer_completion_kind::stopped);
  post_timer_driver();
  return count;
}

io_context::time_point io_context::timer_expiry(
    const detail::timer_slot& timer) const noexcept {
  std::lock_guard context_lock(timers_.mutex);
  std::lock_guard timer_lock(timer.mutex);
  return timer.expiry;
}

void io_context::start_timer_wait(detail::timer_operation_base& operation,
                                  detail::timer_slot& timer) noexcept {
  push_timer_operation(timer.submitted_head, operation);
  post_timer_driver();
}

steady_timer::steady_timer(io_context& context) noexcept {
  timer_.expiry = clock::now();
  context.register_timer(timer_);
}

steady_timer::steady_timer(io_context& context, time_point expiry) noexcept {
  timer_.expiry = expiry;
  context.register_timer(timer_);
}

steady_timer::~steady_timer() noexcept {
  if (timer_.context != nullptr) {
    timer_.context->unregister_timer(timer_);
  }
}

steady_timer::steady_timer(steady_timer&& other) noexcept {
  io_context* context = other.timer_.context;
  time_point expiry = clock::now();
  if (context != nullptr) {
    expiry = context->timer_expiry(other.timer_);
    context->unregister_timer(other.timer_);
    timer_.expiry = expiry;
    context->register_timer(timer_);
  }
}

steady_timer& steady_timer::operator=(steady_timer&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  if (timer_.context != nullptr) {
    timer_.context->unregister_timer(timer_);
  }

  io_context* context = other.timer_.context;
  if (context != nullptr) {
    const time_point expiry = context->timer_expiry(other.timer_);
    context->unregister_timer(other.timer_);
    timer_.expiry = expiry;
    context->register_timer(timer_);
  }
  return *this;
}

steady_timer::time_point steady_timer::expiry() const noexcept {
  return timer_.context->timer_expiry(timer_);
}

std::size_t steady_timer::expires_at(time_point expiry) noexcept {
  return timer_.context->set_timer_expiry(timer_, expiry);
}

std::size_t steady_timer::cancel() noexcept {
  return timer_.context->cancel_timer(timer_);
}

}  // namespace bnio
