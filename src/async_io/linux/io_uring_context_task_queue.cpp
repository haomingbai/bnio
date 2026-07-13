#include <poll.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>

#include "io_uring_context_internal.h"

namespace bupp::async_io::linux_native {

int io_uring_context::post(io_uring_operation_base& operation) noexcept {
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

void io_uring_context::publish_io(
    io_uring_io_operation_base& operation) noexcept {
  assert_running();
  if (operation.ring_affine()) {
    assert(current_context_ == this);
    if (current_context_ != this) {
      return;
    }
  } else if (global_state_ != nullptr) {
    global_state_->push_io(operation);
    notify_one_waiter();
    return;
  }

  assert(!run_active_.load(std::memory_order_acquire) ||
         current_context_ == this);
  operation.io_next = local_io_tasks_;
  local_io_tasks_ = &operation;
}

void io_uring_context::push_cpu_task(
    io_uring_operation_base& operation) noexcept {
  assert(global_state_ != nullptr);
  global_state_->push_cpu(operation);
  notify_one_waiter();
}

void io_uring_context::push_cpu_tasks(operation_queue& operations) noexcept {
  if (global_state_ == nullptr) {
    local_tasks_.push(reverse_tasks(operations.pop_all()));
    return;
  }
  io_uring_operation_base* ordered_tasks = reverse_tasks(operations.pop_all());
  if (ordered_tasks == nullptr) {
    return;
  }

  while (ordered_tasks != nullptr) {
    io_uring_operation_base* operation = ordered_tasks;
    ordered_tasks = ordered_tasks->next;
    operation->next = nullptr;
    global_state_->push_cpu(*operation);
  }
  notify_one_waiter();
}

bool io_uring_context::move_cpu_tasks() noexcept {
  if (global_state_ == nullptr) {
    return false;
  }
  io_uring_operation_base* incoming = global_state_->pop_cpu_all();
  if (incoming == nullptr) {
    return false;
  }

  local_tasks_.push(reverse_tasks(incoming));
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
  if (global_state_ == nullptr) {
    return;
  }
  const std::size_t previous =
      global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
  assert(previous != 0);
}

void io_uring_context::end_wait() noexcept {
  if (global_state_ != nullptr) {
    global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  }
  waiting_.store(false, std::memory_order_release);
}

int io_uring_context::signal_eventfd() noexcept {
  if (event_fd_ < 0) {
    return -EINVAL;
  }

  const std::uint64_t value = 1;
  const auto* bytes = reinterpret_cast<const char*>(&value);
  std::size_t offset = 0;
  while (offset < sizeof(value)) {
    const ssize_t result =
        ::write(event_fd_, bytes + offset, sizeof(value) - offset);
    if (result > 0) {
      offset += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0 && errno == EAGAIN) {
      return 0;
    }
    return result < 0 ? -errno : -EIO;
  }
  return 0;
}

void io_uring_context::drain_eventfd() noexcept {
  if (event_fd_ < 0) {
    return;
  }

  for (;;) {
    std::uint64_t value = 0;
    const ssize_t result = ::read(event_fd_, &value, sizeof(value));
    if (result == static_cast<ssize_t>(sizeof(value))) {
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0 && errno == EAGAIN) {
      return;
    }
    return;
  }
}

int io_uring_context::submit_eventfd_poll() noexcept {
  if (!ring_.is_open() || event_fd_ < 0) {
    return -EINVAL;
  }
  if (eventfd_poll_pending_ ||
      state_.load(std::memory_order_acquire) != context_state::running) {
    return 0;
  }

  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    bupp::base::submission_queue_entry sqe = ring_.get_sqe();
    if (sqe.raw() == nullptr) {
      const int submit_result = submit_ring();
      if (submit_result < 0) {
        return submit_result;
      }
      continue;
    }

    sqe.prep_poll_add(event_fd_, static_cast<unsigned>(POLLIN));
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

}  // namespace bupp::async_io::linux_native
