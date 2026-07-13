#pragma once
#ifndef BUPP_SRC_ASYNC_IO_BSD_KQUEUE_CONTEXT_INTERNAL_H_
#define BUPP_SRC_ASYNC_IO_BSD_KQUEUE_CONTEXT_INTERNAL_H_

#include <bupp/async_io/bsd/kqueue_context.h>

namespace bupp::async_io::bsd_native {

[[nodiscard]] inline kqueue_operation_base* reverse_tasks(
    kqueue_operation_base* tasks) noexcept {
  kqueue_operation_base* reversed = nullptr;
  while (tasks != nullptr) {
    kqueue_operation_base* next = tasks->next;
    tasks->next = reversed;
    reversed = tasks;
    tasks = next;
  }
  return reversed;
}

inline void execute_tasks(kqueue_operation_base* tasks) noexcept {
  while (tasks != nullptr) {
    kqueue_operation_base* operation = tasks;
    tasks = tasks->next;
    operation->next = nullptr;
    operation->execute();
  }
}

}  // namespace bupp::async_io::bsd_native

#endif  // BUPP_SRC_ASYNC_IO_BSD_KQUEUE_CONTEXT_INTERNAL_H_
