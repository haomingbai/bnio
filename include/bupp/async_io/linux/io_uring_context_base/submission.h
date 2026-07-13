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
  assert_ring_owner();
  return prepare_sqe(operation);
}

template <class Operation>
int io_uring_context::submit(Operation& operation) noexcept {
  assert_running();
  assert_ring_owner();

  const int prepare_result = prepare_sqe(operation);
  return prepare_result < 0 ? prepare_result : submit_ring();
}

template <class Function>
void io_uring_context::submit_batch(Function&& fn) noexcept {
  assert_running();
  assert_ring_owner();

  auto prepare = [this](auto& operation) noexcept {
    return this->prepare_sqe(operation);
  };
  auto submit = [this]() noexcept { return submit_ring(); };
  std::forward<Function>(fn)(prepare, submit);
}

template <class Operation>
int io_uring_context::prepare_sqe(Operation& operation) noexcept {
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
