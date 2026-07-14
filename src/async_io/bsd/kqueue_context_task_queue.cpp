#include <cassert>
#include <cerrno>

#include "kqueue_context_internal.h"

namespace bupp::async_io::bsd_native {

int kqueue_context::post(kqueue_operation_base& operation) noexcept {
  assert_running();

  if (current_context_ == this || global_state_ == nullptr) {
    assert(!run_active_.load(std::memory_order_acquire) ||
           current_context_ == this);
    local_tasks_.push(operation);
    return 0;
  }

  push_cpu_task(operation);
  return 0;
}

void kqueue_context::publish_io(kqueue_io_operation_base& operation) noexcept {
  assert_running();
  if (global_state_ != nullptr) {
    global_state_->push_io(operation);
    notify_one_waiter();
    return;
  }

  assert(!run_active_.load(std::memory_order_acquire) ||
         current_context_ == this);
  operation.io_next = local_io_tasks_;
  local_io_tasks_ = &operation;
}

void kqueue_context::push_cpu_task(kqueue_operation_base& operation) noexcept {
  assert(global_state_ != nullptr);
  global_state_->push_cpu(operation);
  notify_one_waiter();
}

void kqueue_context::push_cpu_tasks(operation_queue& operations) noexcept {
  if (global_state_ == nullptr) {
    local_tasks_.push(reverse_tasks(operations.pop_all()));
    return;
  }
  kqueue_operation_base* ordered_tasks = reverse_tasks(operations.pop_all());
  if (ordered_tasks == nullptr) {
    return;
  }

  while (ordered_tasks != nullptr) {
    kqueue_operation_base* operation = ordered_tasks;
    ordered_tasks = ordered_tasks->next;
    operation->next = nullptr;
    global_state_->push_cpu(*operation);
  }
  notify_one_waiter();
}

bool kqueue_context::move_cpu_tasks() noexcept {
  if (global_state_ == nullptr) {
    return false;
  }
  kqueue_operation_base* incoming = global_state_->pop_cpu_all();
  if (incoming == nullptr) {
    return false;
  }

  local_tasks_.push(reverse_tasks(incoming));
  return true;
}

void kqueue_context::notify_one_waiter() noexcept {
  if (is_waiting()) {
    (void)trigger_wakeup();
  }
}

bool kqueue_context::is_waiting() const noexcept {
  return waiting_.load(std::memory_order_acquire);
}

void kqueue_context::begin_wait() noexcept {
  waiting_.store(true, std::memory_order_release);
  if (global_state_ == nullptr) {
    return;
  }
  [[maybe_unused]] const std::size_t previous =
      global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
  assert(previous != 0);
}

void kqueue_context::end_wait() noexcept {
  if (global_state_ != nullptr) {
    global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  }
  waiting_.store(false, std::memory_order_release);
}

int kqueue_context::trigger_wakeup() noexcept {
  if (!queue_.is_open()) {
    return -EINVAL;
  }

  bupp::base::event trigger(options_.wakeup_ident, EVFILT_USER, 0, NOTE_TRIGGER,
                            0, wakeup_user_data());
  return queue_.control(&trigger, 1, nullptr, 0, nullptr);
}

void* kqueue_context::wakeup_user_data() noexcept {
  static int wakeup_sentinel = 0;
  return &wakeup_sentinel;
}

}  // namespace bupp::async_io::bsd_native
