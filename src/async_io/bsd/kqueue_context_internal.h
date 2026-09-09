#pragma once
#ifndef BNIO_SRC_ASYNC_IO_BSD_KQUEUE_CONTEXT_INTERNAL_H_
#define BNIO_SRC_ASYNC_IO_BSD_KQUEUE_CONTEXT_INTERNAL_H_

#include <bnio/async_io/bsd/kqueue_context.h>

namespace bnio::async_io::bsd_native {

inline void kqueue_context::operation_queue::push(
    kqueue_operation_base& operation) noexcept {
  operation.next = head;
  head = &operation;
}

inline void kqueue_context::operation_queue::push(
    kqueue_operation_base* operations) noexcept {
  while (operations != nullptr) {
    kqueue_operation_base* operation = operations;
    operations = operations->next;
    operation->next = nullptr;
    push(*operation);
  }
}

inline kqueue_operation_base*
kqueue_context::operation_queue::pop_all() noexcept {
  kqueue_operation_base* operations = head;
  head = nullptr;
  return operations;
}

inline void execute_tasks(kqueue_operation_base* tasks) noexcept {
  while (tasks != nullptr) {
    kqueue_operation_base* operation = tasks;
    tasks = tasks->next;
    operation->next = nullptr;
    operation->execute();
  }
}

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_SRC_ASYNC_IO_BSD_KQUEUE_CONTEXT_INTERNAL_H_
