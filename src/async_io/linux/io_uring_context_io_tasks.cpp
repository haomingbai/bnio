#include <bnio/async_io/linux/io_uring_context.h>
#include <bnio/base/linux/submission_queue_entry.h>

#include <cassert>
#include <cerrno>

#include "io_uring_context_internal.h"

namespace bnio::async_io::linux_native {

bool io_uring_context::consume_io_tasks() noexcept {
  io_uring_io_operation_base* operations = global_state_->pop_io_all();
  if (operations == nullptr) {
    return false;
  }

  io_uring_io_operation_base* prepared = nullptr;
  const auto release_prepared = [&prepared]() noexcept {
    while (prepared != nullptr) {
      io_uring_io_operation_base* operation = prepared;
      prepared = static_cast<io_uring_io_operation_base*>(operation->next);
      operation->next = nullptr;
    }
  };
  const auto fail_prepared = [this, &prepared](int result) noexcept {
    while (prepared != nullptr) {
      io_uring_io_operation_base* operation = prepared;
      prepared = static_cast<io_uring_io_operation_base*>(operation->next);
      operation->next = nullptr;
      operation->complete_submit_error(result);
      local_tasks_.push(*operation);
    }
  };

  while (operations != nullptr) {
    io_uring_io_operation_base* operation = operations;
    const int prepare_result = prepare_io(*operation);
    if (prepare_result == -EAGAIN) {
      const int submit_result = submit_ring();
      if (submit_result >= 0) {
        release_prepared();
        continue;
      }
      fail_prepared(submit_result);
      while (operations != nullptr) {
        operation = operations;
        operations = static_cast<io_uring_io_operation_base*>(operation->next);
        operation->next = nullptr;
        operation->complete_submit_error(submit_result);
        local_tasks_.push(*operation);
      }
      return true;
    }

    operations = static_cast<io_uring_io_operation_base*>(operation->next);
    operation->next = nullptr;
    if (prepare_result < 0) {
      operation->complete_submit_error(prepare_result);
      local_tasks_.push(*operation);
    } else {
      operation->next = prepared;
      prepared = operation;
    }
  }

  if (prepared != nullptr) {
    const int submit_result = submit_ring();
    if (submit_result >= 0) {
      release_prepared();
    } else {
      fail_prepared(submit_result);
    }
  }
  return true;
}

int io_uring_context::prepare_io(
    io_uring_io_operation_base& operation) noexcept {
  if (!ring_.is_open()) {
    return -EINVAL;
  }

  bnio::base::submission_queue_entry sqe = ring_.get_sqe();
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

int io_uring_context::enable_ring() noexcept {
  if (!ring_.is_open()) {
    return -EINVAL;
  }
  if (!ring_disabled_) {
    return 0;
  }

  const int result = ring_.enable();
  if (result >= 0) {
    ring_disabled_ = false;
  }
  return result;
}

void io_uring_context::assert_running() const noexcept {
#ifndef NDEBUG
  assert(state_.load(std::memory_order_acquire) == context_state::running);
#endif
}

}  // namespace bnio::async_io::linux_native
