#include <bupp/async_io/bsd/kqueue_context.h>

#include <cassert>
#include <cerrno>
#include <limits>
#include <mutex>
#include <new>

namespace bupp::async_io::bsd_native {

thread_local kqueue_context* kqueue_context::current_context_ = nullptr;

kqueue_context::kqueue_context() noexcept = default;

kqueue_context::kqueue_context(const kqueue_context_options& options) noexcept {
  (void)queue_init(options);
}

kqueue_context::~kqueue_context() noexcept { queue_exit(); }

void kqueue_context::apply_context_options(
    const kqueue_context_options& options) noexcept {
  prepared_capacity_ = options.entries == 0 ? 1 : options.entries;
  event_batch_window_ =
      options.event_batch_window == 0 ? 1 : options.event_batch_window;
  wait_spin_count_ = options.wait_spin_count;
  event_inline_completion_threshold_ =
      options.event_inline_completion_threshold;
  local_queue_threshold_ = options.local_queue_threshold;
  wakeup_ident_ = options.wakeup_ident;
}

int kqueue_context::queue_init(const kqueue_context_options& options) noexcept {
  if (queue_initialized_) {
    return -EALREADY;
  }

  apply_context_options(options);
  if (prepared_capacity_ >
      std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(2)) {
    return -ENOMEM;
  }
  const std::size_t active_capacity = prepared_capacity_ * 2;
  auto prepared = std::unique_ptr<prepared_operation[]>(
      new (std::nothrow) prepared_operation[prepared_capacity_]);
  auto active = std::unique_ptr<active_registration[]>(
      new (std::nothrow) active_registration[active_capacity]);
  auto events = std::unique_ptr<bupp::base::event[]>(
      new (std::nothrow) bupp::base::event[event_batch_window_]);
  if (!prepared || !active || !events) {
    return -ENOMEM;
  }

  const int open_result = queue_.open();
  if (open_result < 0) {
    return open_result;
  }

  bupp::base::event wakeup_event(wakeup_ident_, EVFILT_USER, EV_ADD | EV_CLEAR,
                                 0, 0, wakeup_user_data());
  const int wakeup_result =
      queue_.control(&wakeup_event, 1, nullptr, 0, nullptr);
  if (wakeup_result < 0) {
    queue_.close();
    return wakeup_result;
  }

  {
    std::lock_guard lock(posted_tasks_mutex_);
    (void)posted_tasks_.pop_all();
  }
  {
    std::lock_guard lock(submission_mutex_);
    prepared_count_ = 0;
    prepared_operations_ = std::move(prepared);
  }
  {
    std::lock_guard lock(registrations_mutex_);
    active_registration_capacity_ = active_capacity;
    active_registrations_ = std::move(active);
  }
  event_buffer_ = std::move(events);
  run_active_.store(false, std::memory_order_release);
  queue_initialized_ = true;
  state_.store(context_state::running, std::memory_order_release);
  return 0;
}

void kqueue_context::queue_exit() noexcept {
  state_.store(context_state::finished, std::memory_order_release);
  (void)trigger_wakeup();

  {
    std::lock_guard lock(posted_tasks_mutex_);
    (void)posted_tasks_.pop_all();
  }
  {
    std::lock_guard lock(submission_mutex_);
    prepared_count_ = 0;
    prepared_operations_.reset();
  }
  {
    std::lock_guard lock(registrations_mutex_);
    active_registrations_.reset();
    active_registration_capacity_ = 0;
  }

  event_buffer_.reset();
  queue_.close();
  prepared_capacity_ = 0;
  queue_initialized_ = false;
}

bool kqueue_context::is_open() const noexcept { return queue_.is_open(); }

void kqueue_context::assert_running() const noexcept {
#ifndef NDEBUG
  assert(state_.load(std::memory_order_acquire) == context_state::running);
#endif
}

}  // namespace bupp::async_io::bsd_native
