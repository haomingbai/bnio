#include <cerrno>
#include <mutex>

#include "kqueue_context_internal.h"

namespace bupp::async_io::bsd_native {

int kqueue_context::post(kqueue_operation_base& operation) noexcept {
  assert_running();

  if (current_context_ == this && local_tasks_ != nullptr) {
    local_tasks_->push(operation);
    return 0;
  }

  push_posted_task(operation);
  return 0;
}

void kqueue_context::push_posted_task(
    kqueue_operation_base& operation) noexcept {
  {
    std::lock_guard lock(posted_tasks_mutex_);
    posted_tasks_.push(operation);
  }
  notify_one_waiter();
}

void kqueue_context::push_posted_tasks(operation_queue& operations) noexcept {
  kqueue_operation_base* ordered_tasks = reverse_tasks(operations.pop_all());
  if (ordered_tasks == nullptr) {
    return;
  }

  {
    std::lock_guard lock(posted_tasks_mutex_);
    while (ordered_tasks != nullptr) {
      kqueue_operation_base* operation = ordered_tasks;
      ordered_tasks = ordered_tasks->next;
      operation->next = nullptr;
      posted_tasks_.push(*operation);
    }
  }
  notify_one_waiter();
}

bool kqueue_context::move_posted_tasks(operation_queue& local_tasks) noexcept {
  kqueue_operation_base* incoming = nullptr;
  {
    std::lock_guard lock(posted_tasks_mutex_);
    incoming = posted_tasks_.pop_all();
  }
  if (incoming == nullptr) {
    return false;
  }

  local_tasks.push(reverse_tasks(incoming));
  return true;
}

void kqueue_context::notify_waiters() noexcept { (void)trigger_wakeup(); }

void kqueue_context::notify_one_waiter() noexcept { (void)trigger_wakeup(); }

int kqueue_context::trigger_wakeup() noexcept {
  if (!queue_.is_open()) {
    return -EINVAL;
  }

  bupp::base::event trigger(wakeup_ident_, EVFILT_USER, 0, NOTE_TRIGGER, 0,
                            wakeup_user_data());
  return queue_.control(&trigger, 1, nullptr, 0, nullptr);
}

void* kqueue_context::wakeup_user_data() noexcept {
  static int wakeup_sentinel = 0;
  return &wakeup_sentinel;
}

}  // namespace bupp::async_io::bsd_native
