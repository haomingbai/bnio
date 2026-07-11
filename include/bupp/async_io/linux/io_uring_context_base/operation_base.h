#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPERATION_BASE_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPERATION_BASE_H_

#include <bupp/export.h>

namespace bupp::async_io::linux_native {

struct operation_stack_state;

/**
 * Base class for operations scheduled by an io_uring_context.
 */
class BUPP_EXPORT io_uring_operation_base {
 public:
  /**
   * Intrusive next pointer used by context task queues.
   */
  io_uring_operation_base* next = nullptr;

  /**
   * Stack state that currently owns or is executing this operation.
   */
  operation_stack_state* stack_state = nullptr;

  /**
   * Completion result copied from the CQE.
   *
   * @see io_uring_cqe
   */
  int result = 0;

  /**
   * Completion flags copied from the CQE.
   *
   * @see io_uring_cqe
   */
  unsigned flags = 0;

  /**
   * Creates an unlinked operation base.
   */
  io_uring_operation_base() noexcept = default;

  /**
   * Copy construction is disabled because operations are queued intrusively.
   */
  io_uring_operation_base(const io_uring_operation_base&) = delete;

  /**
   * Copy assignment is disabled because operations are queued intrusively.
   */
  io_uring_operation_base& operator=(const io_uring_operation_base&) = delete;

  /**
   * Move construction is disabled because operations are queued intrusively.
   */
  io_uring_operation_base(io_uring_operation_base&&) = delete;

  /**
   * Move assignment is disabled because operations are queued intrusively.
   */
  io_uring_operation_base& operator=(io_uring_operation_base&&) = delete;

  /**
   * Destroys the operation base.
   */
  virtual ~io_uring_operation_base() = default;

  /**
   * Completes the operation on the context run loop.
   */
  virtual void execute() noexcept = 0;
};

/**
 * Non-atomic intrusive stack of operations.
 */
struct BUPP_EXPORT operation_stack_state {
  /**
   * Pushes one operation onto this stack.
   */
  void push(io_uring_operation_base& operation) noexcept {
    operation.next = head;
    operation.stack_state = this;
    head = &operation;
  }

  /**
   * Pushes a forward-linked operation chain onto this stack.
   */
  void push(io_uring_operation_base* operations) noexcept {
    while (operations != nullptr) {
      io_uring_operation_base* operation = operations;
      operations = operations->next;
      operation->next = nullptr;
      push(*operation);
    }
  }

  /**
   * Removes and returns all operations currently on this stack.
   */
  [[nodiscard]] io_uring_operation_base* pop_all() noexcept {
    io_uring_operation_base* operations = head;
    head = nullptr;
    return operations;
  }

  /**
   * Returns whether the stack is empty.
   */
  [[nodiscard]] bool empty() const noexcept { return head == nullptr; }

  io_uring_operation_base* head = nullptr;
};

}  // namespace bupp::async_io::linux_native

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPERATION_BASE_H_
