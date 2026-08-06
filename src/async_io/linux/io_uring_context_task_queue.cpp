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
    local_state_.push_cpu(operation);
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

io_uring_operation_base* io_uring_context::fetch_cpu_task() noexcept {
  // 1. Worker-local queue first: fastest and preserves locality.
  if (io_uring_operation_base* operations = local_state_.pop_cpu_all()) {
    return reverse_tasks(operations);
  }

  // 2. Shared CPU queue.
  if (io_uring_operation_base* operations = global_state_->pop_cpu_all()) {
    return reverse_tasks(operations);
  }

  // 3. Steal from another worker's local queue. Only reached when both
  //    local and shared queues are empty, so a stealing worker is by
  //    definition relatively idle.
  return steal_cpu_tasks();
}

bool io_uring_context::run_one_cpu_task() noexcept {
  if (io_uring_operation_base* operations = fetch_cpu_task()) {
    execute_tasks(operations);
    return true;
  }
  return false;
}

io_uring_operation_base* io_uring_context::steal_cpu_tasks() noexcept {
  // The lock guards both the list traversal and the lifetime of every node:
  // a worker unregisters its local state under the same lock before its
  // context (and local state) is destroyed, so a visited node is never freed
  // while we touch it (UAF protection).
  std::lock_guard<std::mutex> guard(global_state_->local_states_lock);

  // Validate the saved cursor is still in the list; otherwise restart from
  // the head. Head insertion never moves existing nodes, so a valid cursor
  // keeps its position without re-traversal.
  io_uring_local_task_queue_state* start = global_state_->local_states;
  if (steal_cursor_ != nullptr) {
    io_uring_local_task_queue_state* scan = start;
    while (scan != nullptr && scan != steal_cursor_) {
      scan = scan->next;
    }
    if (scan != nullptr) {
      start = steal_cursor_;
    } else {
      steal_cursor_ = nullptr;
    }
  }

  // Traverse the list starting at the cursor, stealing the first non-empty
  // local queue in bulk. Stop as soon as a batch is stolen.
  io_uring_local_task_queue_state* node = start;
  do {
    if (node != &local_state_) {
      if (io_uring_operation_base* operations = node->pop_cpu_all()) {
        steal_cursor_ = node->next;
        return reverse_tasks(operations);
      }
    }
    node = node->next;
  } while (node != nullptr);

  steal_cursor_ = nullptr;
  return nullptr;
}

void io_uring_context::register_local_state() noexcept {
  std::lock_guard<std::mutex> guard(global_state_->local_states_lock);
  local_state_.next = global_state_->local_states;
  global_state_->local_states = &local_state_;
}

void io_uring_context::unregister_local_state() noexcept {
  std::lock_guard<std::mutex> guard(global_state_->local_states_lock);
  io_uring_local_task_queue_state** link = &global_state_->local_states;
  while (*link != nullptr && *link != &local_state_) {
    link = &(*link)->next;
  }
  if (*link == &local_state_) {
    *link = local_state_.next;
    local_state_.next = nullptr;
  }
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

int io_uring_context::submit_local_eventfd_poll() noexcept {
  const int local_fd = local_state_.wake_channel_.is_open()
                           ? local_state_.wake_channel_.read_fd()
                           : -1;
  if (!ring_.is_open() || local_fd < 0) {
    return -EINVAL;
  }
  if (local_eventfd_poll_pending_ ||
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

    sqe.prep_poll_add(local_fd, static_cast<unsigned>(POLLIN));
    sqe.set_data(local_eventfd_user_data());

    const int submit_result = submit_ring();
    if (submit_result <= 0) {
      return submit_result < 0 ? submit_result : -EAGAIN;
    }

    local_eventfd_poll_pending_ = true;
    return submit_result;
  }

  return -EAGAIN;
}

void* io_uring_context::local_eventfd_user_data() noexcept {
  static int local_eventfd_sentinel = 0;
  return &local_eventfd_sentinel;
}

}  // namespace bnio::async_io::linux_native
