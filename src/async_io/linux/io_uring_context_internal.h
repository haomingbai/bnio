#pragma once
#ifndef BUPP_SRC_ASYNC_IO_LINUX_IO_URING_CONTEXT_INTERNAL_H_
#define BUPP_SRC_ASYNC_IO_LINUX_IO_URING_CONTEXT_INTERNAL_H_

#include <bupp/async_io/linux/io_uring_context.h>

namespace bupp::async_io::linux_native {

inline void io_uring_context::operation_queue::push(
    io_uring_operation_base& operation) noexcept {
  operation.next = head;
  head = &operation;
}

inline void io_uring_context::operation_queue::push(
    io_uring_operation_base* operations) noexcept {
  while (operations != nullptr) {
    io_uring_operation_base* operation = operations;
    operations = operations->next;
    operation->next = nullptr;
    push(*operation);
  }
}

inline io_uring_operation_base*
io_uring_context::operation_queue::pop_all() noexcept {
  io_uring_operation_base* operations = head;
  head = nullptr;
  return operations;
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
