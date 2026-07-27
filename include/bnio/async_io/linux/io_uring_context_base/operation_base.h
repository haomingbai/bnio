/**
 * @file operation_base.h
 * @brief Base classes for io_uring operations.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPERATION_BASE_H_
#define BNIO_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPERATION_BASE_H_

#include <bnio/async_io/time.h>
#include <bnio/base/wake_channel.h>
#include <bnio/export.h>

#include <atomic>
#include <cstddef>

namespace bnio::base {
class submission_queue_entry;
}

namespace bnio::async_io::linux_native {

class io_uring_operation_base;
class io_uring_io_operation_base;

/** Shared MPSC CPU/I/O queues and worker-group lifecycle state. */
struct BNIO_EXPORT io_uring_task_queue_state {
  using try_fetch_timeout_fn = bool (*)(void*, async_io::time_point&,
                                        io_uring_operation_base*&) noexcept;

  /** Drains all ready native completions from the ring without blocking. */
  using drain_ready_native_fn = io_uring_operation_base*
      (*)(void* ctx) noexcept;

  void push_cpu(io_uring_operation_base& operation) noexcept;

  [[nodiscard]] io_uring_operation_base* pop_cpu_all() noexcept;

  void push_io(io_uring_io_operation_base& operation) noexcept;

  [[nodiscard]] io_uring_io_operation_base* pop_io_all() noexcept;

  std::atomic<io_uring_operation_base*> cpu_head{nullptr};
  std::atomic<io_uring_io_operation_base*> io_head{nullptr};
  std::atomic<std::size_t> awake_workers{0};
  std::atomic_bool closing{false};

  /** Opaque shared lazy timer heap and its non-blocking fetch entry point. */
  void* timeout_heap = nullptr;
  try_fetch_timeout_fn try_fetch_timeout_operations = nullptr;

  /** Shared wake channel owned by io_context.
   *
   * io_context creates the channel once and writes to it to wake
   * workers.  Each worker's io_uring_context registers read interest
   * (IORING_POLL_ADD) before sleeping.
   *
   * \warning A single ::write wakes ALL workers whose rings have a
   * pending IORING_POLL_ADD on this fd (minor thundering herd). The
   * per-worker overhead is one ::read → EAGAIN plus one
   * pop_cpu_all() CAS — negligible for typical 4–8 worker
   * concurrency.  During stop(), waking all workers is the desired
   * behaviour.
   */
  bnio::base::wake_channel wake_channel_;

  /** Unified drain: collects all ready native completions (CQEs /
   *  kevents) into a linked list. Registered by each native_context
   *  in set_global_state().
   */
  drain_ready_native_fn drain_ready_native = nullptr;
  void* drain_native_context = nullptr;
};

/**
 * Base class for operations scheduled by an io_uring_context.
 */
class BNIO_EXPORT io_uring_operation_base {
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
class BNIO_EXPORT io_uring_io_operation_base : public io_uring_operation_base {
 public:
  io_uring_io_operation_base() noexcept = default;
  io_uring_io_operation_base(const io_uring_io_operation_base&) = delete;
  io_uring_io_operation_base& operator=(const io_uring_io_operation_base&) =
      delete;
  io_uring_io_operation_base(io_uring_io_operation_base&&) = delete;
  io_uring_io_operation_base& operator=(io_uring_io_operation_base&&) = delete;
  ~io_uring_io_operation_base() override = default;

  /** Fills one SQE after the run loop takes this operation from the queue. */
  virtual void prepare(bnio::base::submission_queue_entry& sqe) noexcept = 0;

  /** Selects the completion delivered when SQE preparation fails. */
  virtual void complete_submit_error(int result) noexcept = 0;
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
    operation.next = head;
  } while (!io_head.compare_exchange_weak(
      head, &operation, std::memory_order_release, std::memory_order_relaxed));
}

inline io_uring_io_operation_base*
io_uring_task_queue_state::pop_io_all() noexcept {
  return io_head.exchange(nullptr, std::memory_order_acquire);
}

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPERATION_BASE_H_
