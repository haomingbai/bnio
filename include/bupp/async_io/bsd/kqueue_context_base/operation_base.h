#pragma once
#ifndef BUPP_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPERATION_BASE_H_
#define BUPP_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPERATION_BASE_H_

#include <bupp/async_io/buffer_view.h>
#include <bupp/export.h>

namespace bupp::async_io::bsd_native {

struct kqueue_operation_stack_state;

/**
 * Base class for operations scheduled by a kqueue_context.
 *
 * Descriptor and readiness metadata intentionally live in the context, so
 * CPU-only operations do not carry unused native I/O fields.
 */
class BUPP_EXPORT kqueue_operation_base {
 public:
  /** Intrusive next pointer used by context task queues. */
  kqueue_operation_base* next = nullptr;

  /** Stack state that currently owns or executes this operation. */
  kqueue_operation_stack_state* stack_state = nullptr;

  /** Native completion result, or a negative errno. */
  int result = 0;

  /** Native kevent flags copied from the readiness notification. */
  unsigned flags = 0;

  kqueue_operation_base() noexcept = default;
  kqueue_operation_base(const kqueue_operation_base&) = delete;
  kqueue_operation_base& operator=(const kqueue_operation_base&) = delete;
  kqueue_operation_base(kqueue_operation_base&&) = delete;
  kqueue_operation_base& operator=(kqueue_operation_base&&) = delete;
  virtual ~kqueue_operation_base() = default;

  /** Completes the operation on the context run loop. */
  virtual void execute() noexcept = 0;

  /**
   * Returns the buffer used by context-owned read/write work.
   *
   * Non-buffered operations return an empty view. Returning the view by value
   * keeps the interface non-owning and allocation-free.
   */
  [[nodiscard]] virtual buffer_view get_data() noexcept { return {}; }
};

/** Non-atomic intrusive stack of kqueue operations. */
struct BUPP_EXPORT kqueue_operation_stack_state {
  void push(kqueue_operation_base& operation) noexcept {
    operation.next = head;
    operation.stack_state = this;
    head = &operation;
  }

  void push(kqueue_operation_base* operations) noexcept {
    while (operations != nullptr) {
      kqueue_operation_base* operation = operations;
      operations = operations->next;
      operation->next = nullptr;
      push(*operation);
    }
  }

  [[nodiscard]] kqueue_operation_base* pop_all() noexcept {
    kqueue_operation_base* operations = head;
    head = nullptr;
    return operations;
  }

  [[nodiscard]] bool empty() const noexcept { return head == nullptr; }

  kqueue_operation_base* head = nullptr;
};

}  // namespace bupp::async_io::bsd_native

#endif  // BUPP_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPERATION_BASE_H_
