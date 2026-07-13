#include <bupp/async_io/linux/io_uring_context.h>
#include <bupp/base/linux/submission_queue_entry.h>

#include <cassert>
#include <cerrno>
#include <utility>

#include "io_uring_context_internal.h"

namespace bupp::async_io::linux_native {

namespace {

// MPSC publication is LIFO; restore producer order before preparing SQEs.
io_uring_io_operation_base* reverse_io_tasks(
    io_uring_io_operation_base* tasks) noexcept {
  io_uring_io_operation_base* reversed = nullptr;
  while (tasks != nullptr) {
    io_uring_io_operation_base* next = tasks->io_next;
    tasks->io_next = reversed;
    reversed = tasks;
    tasks = next;
  }
  return reversed;
}

}  // namespace

bool io_uring_context::consume_io_tasks() noexcept {
  io_uring_io_operation_base* ring_local =
      std::exchange(local_io_tasks_, nullptr);
  io_uring_io_operation_base* global_io =
      global_state_ == nullptr ? nullptr : global_state_->pop_io_all();

  io_uring_io_operation_base* operations = reverse_io_tasks(ring_local);
  io_uring_io_operation_base** tail = &operations;
  while (*tail != nullptr) {
    tail = &(*tail)->io_next;
  }
  *tail = reverse_io_tasks(global_io);
  if (operations == nullptr) {
    return false;
  }

  while (operations != nullptr) {
    io_uring_io_operation_base* operation = operations;
    const int prepare_result = prepare_io(*operation);
    if (prepare_result == -EAGAIN) {
      const int submit_result = submit_ring();
      if (submit_result >= 0) {
        continue;
      }
      while (operations != nullptr) {
        operation = operations;
        operations = operation->io_next;
        operation->io_next = nullptr;
        operation->complete_submit_error(submit_result);
        local_tasks_.push(*operation);
      }
      return true;
    }

    operations = operation->io_next;
    operation->io_next = nullptr;
    if (prepare_result < 0) {
      operation->complete_submit_error(prepare_result);
      local_tasks_.push(*operation);
    }
  }

  (void)submit_ring();
  return true;
}

int io_uring_context::prepare_io(
    io_uring_io_operation_base& operation) noexcept {
  if (!ring_.is_open()) {
    return -EINVAL;
  }

  bupp::base::submission_queue_entry sqe = ring_.get_sqe();
  if (sqe.raw() == nullptr) {
    return -EAGAIN;
  }

  operation.prepare(sqe);
  sqe.set_data(static_cast<io_uring_operation_base*>(&operation));
  return 0;
}

int io_uring_context::submit_ring() noexcept {
  if (!ring_.is_open()) {
    return -EINVAL;
  }
  return ring_.submit();
}

void io_uring_context::assert_running() const noexcept {
#ifndef NDEBUG
  assert(state_.load(std::memory_order_acquire) == context_state::running);
#endif
}

}  // namespace bupp::async_io::linux_native
