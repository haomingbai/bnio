#pragma once
#ifndef BUPP_SRC_ASYNC_IO_LINUX_IO_URING_CONTEXT_INTERNAL_H_
#define BUPP_SRC_ASYNC_IO_LINUX_IO_URING_CONTEXT_INTERNAL_H_

#include <bupp/async_io/linux/io_uring_context.h>

namespace bupp::async_io::linux_native {

[[nodiscard]] inline io_uring_operation_base* reverse_tasks(
    io_uring_operation_base* tasks) noexcept {
  io_uring_operation_base* reversed = nullptr;
  while (tasks != nullptr) {
    io_uring_operation_base* next = tasks->next;
    tasks->next = reversed;
    reversed = tasks;
    tasks = next;
  }
  return reversed;
}

inline void execute_tasks(io_uring_operation_base* tasks) noexcept {
  while (tasks != nullptr) {
    io_uring_operation_base* operation = tasks;
    tasks = tasks->next;
    operation->next = nullptr;
    operation->execute();
  }
}

struct io_uring_context::cqe_data {
  void* user_data = nullptr;
  int result = 0;
  unsigned flags = 0;
};

}  // namespace bupp::async_io::linux_native

#endif  // BUPP_SRC_ASYNC_IO_LINUX_IO_URING_CONTEXT_INTERNAL_H_
