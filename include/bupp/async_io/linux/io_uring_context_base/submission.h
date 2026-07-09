#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_SUBMISSION_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_SUBMISSION_H_

#include <bupp/async_io/linux/io_uring_context_base/context.h>
#include <bupp/base/linux/submission_queue_entry.h>

#include <cassert>
#include <cerrno>
#include <utility>

namespace bupp::async_io::linux_native {

/** @cond BUPP_DETAIL */

template <class Operation>
int io_uring_context::prepare(Operation& operation) noexcept {
  assert_running();
  auto lock = lock_uring();
  return prepare_locked(operation);
}

template <class Operation>
int io_uring_context::submit(Operation& operation) noexcept {
  assert_running();

  {
    auto lock = lock_uring();
    const int prepare_result = prepare_locked(operation);
    if (prepare_result < 0) {
      return prepare_result;
    }
    return submit_locked();
  }
}

template <class Function>
void io_uring_context::submit_batch(Function&& fn) noexcept {
  assert_running();

  {
    auto lock = lock_uring();
    auto prepare = [this](auto& operation) noexcept {
      return prepare_locked(operation);
    };
    auto submit = [this]() noexcept { return submit_locked(); };

    std::forward<Function>(fn)(prepare, submit);
  }
}

template <class Operation>
int io_uring_context::prepare_locked(Operation& operation) noexcept {
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

/** @endcond */

}  // namespace bupp::async_io::linux_native

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_SUBMISSION_H_
