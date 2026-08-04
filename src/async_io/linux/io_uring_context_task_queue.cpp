/**
 * @file io_uring_context_task_queue.cpp
 * @brief Task queue operations: post, publish, CPU push, wait/notify, and
 * wake channel I/O.
 */

#include <poll.h>

#include <cassert>
#include <cerrno>
#include <mutex>

#include "io_uring_context_internal.h"

namespace bnio::async_io::linux_native {

int io_uring_context::post(io_uring_operation_base& operation) noexcept {
  assert_running();

  if (current_context_ == this) {
    local_tasks_.push(operation);
    return 0;
  }

  global_state_->push_cpu(operation);
  notify_one_waiter();
  return 0;
}

void io_uring_context::publish_io(
    io_uring_io_operation_base& operation) noexcept {
  assert_running();
  global_state_->push_io(operation);
  notify_one_waiter();
}

void io_uring_context::push_cpu_tasks(operation_queue& operations) noexcept {
  io_uring_operation_base* tasks = operations.pop_all();
  if (tasks == nullptr) {
    return;
  }

  while (tasks != nullptr) {
    io_uring_operation_base* operation = tasks;
    tasks = tasks->next;
    operation->next = nullptr;
    global_state_->push_cpu(*operation);
  }
  notify_one_waiter();
}

bool io_uring_context::move_cpu_tasks() noexcept {
  io_uring_operation_base* incoming = global_state_->pop_cpu_all();
  if (incoming == nullptr) {
    return false;
  }

  local_tasks_.push(incoming);
  return true;
}

void io_uring_context::notify_one_waiter() noexcept {
  if (is_waiting()) {
    (void)signal_eventfd();
  }
}

bool io_uring_context::is_waiting() const noexcept {
  return waiting_.load(std::memory_order_acquire);
}

void io_uring_context::begin_wait() noexcept {
  waiting_.store(true, std::memory_order_release);
  [[maybe_unused]] const std::size_t previous =
      global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
  assert(previous != 0);
}

void io_uring_context::end_wait() noexcept {
  global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  waiting_.store(false, std::memory_order_release);
}

int io_uring_context::signal_eventfd() noexcept {
  if (global_state_ == nullptr) {
    return -EINVAL;
  }
  // Bind the shared wake-channel write to the submit lock so the
  // io_context destructor's close (under the same lock) can never race
  // it. Re-check after acquiring the lock: the channel may have been
  // closed while we waited for the lock.
  std::lock_guard guard(global_state_->submit_lock);
  if (!global_state_->wake_channel_.is_open()) {
    return -EBADF;
  }
  return global_state_->wake_channel_.wake();
}

void io_uring_context::drain_eventfd() noexcept {
  if (global_state_ != nullptr) {
    (void)global_state_->wake_channel_.drain();
  }
}

int io_uring_context::submit_eventfd_poll() noexcept {
  const int wake_fd =
      global_state_ != nullptr ? global_state_->wake_channel_.read_fd() : -1;
  if (!ring_.is_open() || wake_fd < 0) {
    return -EINVAL;
  }
  if (eventfd_poll_pending_ ||
      state_.load(std::memory_order_acquire) != context_state::running) {
    return 0;
  }

  // 2-attempt retry: when the SQ is full, submit and try again. After two
  // failures, return EAGAIN so the caller can retry later.
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    bnio::base::submission_queue_entry sqe = ring_.get_sqe();
    if (sqe.raw() == nullptr) {
      const int submit_result = submit_ring();
      if (submit_result < 0) {
        return submit_result;
      }
      continue;
    }

    sqe.prep_poll_add(wake_fd, static_cast<unsigned>(POLLIN));
    sqe.set_data(eventfd_user_data());

    const int submit_result = submit_ring();
    if (submit_result <= 0) {
      return submit_result < 0 ? submit_result : -EAGAIN;
    }

    eventfd_poll_pending_ = true;
    return submit_result;
  }

  return -EAGAIN;
}

void* io_uring_context::eventfd_user_data() noexcept {
  static int eventfd_sentinel = 0;
  return &eventfd_sentinel;
}

}  // namespace bnio::async_io::linux_native
