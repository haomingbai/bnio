#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPERATION_BASE_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPERATION_BASE_H_

#include <bupp/export.h>

#include <atomic>
#include <cstddef>

namespace bupp::base {
class submission_queue_entry;
}

namespace bupp::async_io::linux_native {

class io_uring_operation_base;
class io_uring_io_operation_base;

/** Shared MPSC CPU/I/O queues and worker-group lifecycle state. */
struct BUPP_EXPORT io_uring_task_queue_state {
  void push_cpu(io_uring_operation_base& operation) noexcept;

  [[nodiscard]] io_uring_operation_base* pop_cpu_all() noexcept;

  void push_io(io_uring_io_operation_base& operation) noexcept;

  [[nodiscard]] io_uring_io_operation_base* pop_io_all() noexcept;

  std::atomic<io_uring_operation_base*> cpu_head{nullptr};
  std::atomic<io_uring_io_operation_base*> io_head{nullptr};
  std::atomic<std::size_t> awake_workers{0};
  std::atomic_bool closing{false};
};

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

/** I/O operation passively prepared by an io_uring_context run loop. */
class BUPP_EXPORT io_uring_io_operation_base : public io_uring_operation_base {
 public:
  io_uring_io_operation_base() noexcept = default;
  io_uring_io_operation_base(const io_uring_io_operation_base&) = delete;
  io_uring_io_operation_base& operator=(const io_uring_io_operation_base&) =
      delete;
  io_uring_io_operation_base(io_uring_io_operation_base&&) = delete;
  io_uring_io_operation_base& operator=(io_uring_io_operation_base&&) = delete;
  ~io_uring_io_operation_base() override = default;

  /** Intrusive link used by local and shared I/O queues. */
  io_uring_io_operation_base* io_next = nullptr;

  /** Fills one SQE after the run loop takes this operation from the queue. */
  virtual void prepare(bupp::base::submission_queue_entry& sqe) noexcept = 0;

  /** Selects the completion delivered when SQE preparation fails. */
  virtual void complete_submit_error(int result) noexcept = 0;

  /** Returns whether this internal operation must stay on the current ring. */
  [[nodiscard]] virtual bool ring_affine() const noexcept { return false; }
};

inline void io_uring_task_queue_state::push_cpu(
    io_uring_operation_base& operation) noexcept {
  io_uring_operation_base* head = cpu_head.load(std::memory_order_relaxed);
  do {
    operation.next = head;
  } while (!cpu_head.compare_exchange_weak(
      head, &operation, std::memory_order_release, std::memory_order_relaxed));
}

inline io_uring_operation_base*
io_uring_task_queue_state::pop_cpu_all() noexcept {
  return cpu_head.exchange(nullptr, std::memory_order_acquire);
}

inline void io_uring_task_queue_state::push_io(
    io_uring_io_operation_base& operation) noexcept {
  io_uring_io_operation_base* head = io_head.load(std::memory_order_relaxed);
  do {
    operation.io_next = head;
  } while (!io_head.compare_exchange_weak(
      head, &operation, std::memory_order_release, std::memory_order_relaxed));
}

inline io_uring_io_operation_base*
io_uring_task_queue_state::pop_io_all() noexcept {
  return io_head.exchange(nullptr, std::memory_order_acquire);
}

}  // namespace bupp::async_io::linux_native

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPERATION_BASE_H_
