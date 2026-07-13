#pragma once
#ifndef BUPP_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_SUBMISSION_H_
#define BUPP_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_SUBMISSION_H_

#include <bupp/async_io/bsd/kqueue_context_base/context.h>

#include <cerrno>
#include <mutex>
#include <utility>

namespace bupp::async_io::bsd_native {

/** @cond BUPP_DETAIL */

template <class Operation>
int kqueue_context::prepare(Operation& operation) noexcept {
  assert_running();
  std::lock_guard lock(submission_mutex_);
  return prepare_locked(operation);
}

template <class Operation>
int kqueue_context::submit(Operation& operation) noexcept {
  assert_running();
  std::lock_guard lock(submission_mutex_);
  const int prepare_result = prepare_locked(operation);
  if (prepare_result < 0) {
    return prepare_result;
  }
  return submit_locked();
}

template <class Function>
void kqueue_context::submit_batch(Function&& function) noexcept {
  assert_running();
  std::lock_guard lock(submission_mutex_);
  auto prepare_operation = [this](auto& operation) noexcept {
    return this->prepare_locked(operation);
  };
  auto submit_operations = [this]() noexcept { return this->submit_locked(); };
  std::forward<Function>(function)(prepare_operation, submit_operations);
}

template <class Operation>
int kqueue_context::prepare_locked(Operation& operation) noexcept {
  if (!queue_.is_open()) {
    return -EINVAL;
  }
  if (prepared_count_ >= prepared_capacity_) {
    return -EAGAIN;
  }

  kqueue_helper helper;
  operation.prepare(helper);
  if (helper.error() < 0) {
    return helper.error();
  }
  if (helper.task() == kqueue_task::none) {
    return -EINVAL;
  }

  auto& base_operation = static_cast<kqueue_operation_base&>(operation);
  helper.set_udata(&base_operation);
  prepared_operation& prepared = prepared_operations_[prepared_count_++];
  prepared.operation = &base_operation;
  prepared.event_count = helper.event_count();
  prepared.task = helper.task();
  prepared.poll_mask = helper.poll_mask();
  for (std::size_t index = 0; index < helper.event_count(); ++index) {
    prepared.events[index] = helper.event(index);
  }
  return 0;
}

/** @endcond */

}  // namespace bupp::async_io::bsd_native

#endif  // BUPP_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_SUBMISSION_H_
