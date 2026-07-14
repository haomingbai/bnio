#pragma once
#ifndef BUPP_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPERATION_BASE_H_
#define BUPP_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPERATION_BASE_H_

#include <bupp/async_io/buffer_view.h>
#include <bupp/export.h>

#include <atomic>
#include <cstddef>

namespace bupp::async_io::bsd_native {

class kqueue_helper;
class kqueue_operation_base;
class kqueue_io_operation_base;

/** Shared MPSC CPU/I/O queues and worker-group lifecycle state. */
struct BUPP_EXPORT kqueue_task_queue_state {
  void push_cpu(kqueue_operation_base& operation) noexcept;

  [[nodiscard]] kqueue_operation_base* pop_cpu_all() noexcept;

  void push_io(kqueue_io_operation_base& operation) noexcept;

  [[nodiscard]] kqueue_io_operation_base* pop_io_all() noexcept;

  std::atomic<kqueue_operation_base*> cpu_head{nullptr};
  std::atomic<kqueue_io_operation_base*> io_head{nullptr};
  std::atomic<std::size_t> awake_workers{0};
  std::atomic_bool closing{false};
};

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
};

/** I/O operation passively prepared by a kqueue_context run loop. */
class BUPP_EXPORT kqueue_io_operation_base : public kqueue_operation_base {
 public:
  kqueue_io_operation_base() noexcept = default;
  kqueue_io_operation_base(const kqueue_io_operation_base&) = delete;
  kqueue_io_operation_base& operator=(const kqueue_io_operation_base&) = delete;
  kqueue_io_operation_base(kqueue_io_operation_base&&) = delete;
  kqueue_io_operation_base& operator=(kqueue_io_operation_base&&) = delete;
  ~kqueue_io_operation_base() override = default;

  /** Intrusive link used by local and shared I/O queues. */
  kqueue_io_operation_base* io_next = nullptr;

  /** Describes the native registration after the run loop takes this task. */
  virtual void prepare(kqueue_helper& helper) noexcept = 0;

  /** Selects error completion when preparation or registration fails. */
  virtual void complete_submit_error(int result) noexcept = 0;

  /** Returns the non-owning buffer used by context-owned read/write work. */
  [[nodiscard]] virtual buffer_view get_data() noexcept { return {}; }
};

inline void kqueue_task_queue_state::push_cpu(
    kqueue_operation_base& operation) noexcept {
  kqueue_operation_base* head = cpu_head.load(std::memory_order_relaxed);
  do {
    operation.next = head;
  } while (!cpu_head.compare_exchange_weak(
      head, &operation, std::memory_order_release, std::memory_order_relaxed));
}

inline kqueue_operation_base* kqueue_task_queue_state::pop_cpu_all() noexcept {
  return cpu_head.exchange(nullptr, std::memory_order_acquire);
}

inline void kqueue_task_queue_state::push_io(
    kqueue_io_operation_base& operation) noexcept {
  kqueue_io_operation_base* head = io_head.load(std::memory_order_relaxed);
  do {
    operation.io_next = head;
  } while (!io_head.compare_exchange_weak(
      head, &operation, std::memory_order_release, std::memory_order_relaxed));
}

inline kqueue_io_operation_base*
kqueue_task_queue_state::pop_io_all() noexcept {
  return io_head.exchange(nullptr, std::memory_order_acquire);
}

}  // namespace bupp::async_io::bsd_native

#endif  // BUPP_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPERATION_BASE_H_
