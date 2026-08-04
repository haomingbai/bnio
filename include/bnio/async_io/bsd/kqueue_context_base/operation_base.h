/**
 * @file operation_base.h
 * @brief Base classes for kqueue operations.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPERATION_BASE_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPERATION_BASE_H_

#include <bnio/async_io/time.h>
#include <bnio/base/wake_channel.h>
#include <bnio/export.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace bnio::async_io::bsd_native {

class kqueue_helper;
class kqueue_operation_base;
class kqueue_io_operation_base;

/** Native action selected for a kqueue operation. */
enum class kqueue_task : std::uint8_t {
  none,
  nop,
  read,
  write,
  poll,
};

/**
 * Complete per-(ident, filter) registration state for one kevent.
 *
 * An I/O operation owns up to two of these (EVFILT_READ and EVFILT_WRITE
 * for poll requests). Each node is linked into exactly one wait queue
 * keyed by (ident, filter), so a single poll operation can wait on both
 * filters simultaneously.
 */
struct kqueue_registration_state {
  /** Back-pointer to the owning operation; also the kevent udata value. */
  kqueue_io_operation_base* operation = nullptr;

  /** Descriptor ident shared by every node of the owning operation. */
  std::uintptr_t ident = 0;

  /** Native filter (EVFILT_READ / EVFILT_WRITE). */
  std::int16_t filter = 0;

  /** Native action selected by prepare(). */
  kqueue_task task = kqueue_task::none;

  /** Original poll mask for poll tasks. */
  unsigned poll_mask = 0;

  /** Whether this node currently owns an armed kevent in the kernel. */
  bool armed = false;

  /** Monotonic allocation order; mirrors wait-queue insertion order. */
  std::uint64_t sequence = 0;

  /** Intrusive wait-queue links for the (ident, filter) list. */
  kqueue_registration_state* wait_next = nullptr;
  kqueue_registration_state* wait_prev = nullptr;
};

/** Shared MPSC CPU/I/O queues and worker-group lifecycle state. */
struct BNIO_EXPORT kqueue_task_queue_state {
  using try_fetch_timeout_fn = bool (*)(void*, async_io::time_point&,
                                        kqueue_operation_base*&) noexcept;

  void push_cpu(kqueue_operation_base& operation) noexcept;

  [[nodiscard]] kqueue_operation_base* pop_cpu_all() noexcept;

  void push_io(kqueue_io_operation_base& operation) noexcept;

  [[nodiscard]] kqueue_io_operation_base* pop_io_all() noexcept;

  std::atomic<kqueue_operation_base*> cpu_head{nullptr};
  std::atomic<kqueue_io_operation_base*> io_head{nullptr};
  std::atomic<std::size_t> awake_workers{0};

  std::atomic<int> life_state{0};  // 0 = running, 1 = stopping
  /** Opaque shared lazy timer heap and its non-blocking fetch entry point. */
  void* timeout_heap = nullptr;
  try_fetch_timeout_fn try_fetch_timeout_operations = nullptr;

  /** Shared wake channel owned by io_context.
   *
   * io_context creates the channel once and writes to it to wake
   * workers.  Each worker's kqueue_context registers EVFILT_READ |
   * EV_CLEAR on the read end before sleeping.
   *
   * @warning A single write wakes ALL workers whose kqueues have
   * EVFILT_READ registered on the read end (minor thundering herd).
   * The per-worker overhead is one ::read → EAGAIN plus one
   * pop_cpu_all() CAS — negligible for typical 4–8 worker
   * concurrency.  During stop(), waking all workers is the desired
   * behaviour.
   */
  bnio::base::wake_channel wake_channel_;

  /** Submit-path lock shared by publish_cpu() and begin_stop().
   *
   * Both the "check state + enqueue + wake" submission critical section and
   * the stop-path state transition run inside this lock, so an operation
   * that observed the context as running is guaranteed to be drained by the
   * last worker's finish(), and an operation that observes the stopping
   * state never enqueues into a queue that may no longer be drained.
   */
  std::mutex submit_lock;
};

/**
 * Base class for operations scheduled by a kqueue_context.
 *
 * Descriptor and readiness metadata intentionally live in the context, so
 * CPU-only operations do not carry unused native I/O fields.
 */
class BNIO_EXPORT kqueue_operation_base {
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
class BNIO_EXPORT kqueue_io_operation_base : public kqueue_operation_base {
 public:
  kqueue_io_operation_base() noexcept = default;
  kqueue_io_operation_base(const kqueue_io_operation_base&) = delete;
  kqueue_io_operation_base& operator=(const kqueue_io_operation_base&) = delete;
  kqueue_io_operation_base(kqueue_io_operation_base&&) = delete;
  kqueue_io_operation_base& operator=(kqueue_io_operation_base&&) = delete;
  ~kqueue_io_operation_base() override = default;

  /** Intrusive link used by local and shared I/O queues. */
  kqueue_io_operation_base* io_next = nullptr;

  /** Reverse link for inflight doubly-linked list during shutdown. */
  kqueue_io_operation_base* io_prev = nullptr;

  /**
   * Per-(ident, filter) registration state filled by the run loop.
   *
   * A poll request occupies two entries (READ + WRITE); single-filter
   * operations occupy one. `registration_count` is set by prepare_io()
   * before the operation is registered with the kqueue.
   */
  std::array<kqueue_registration_state, 2> registrations{};
  std::uint8_t registration_count = 0;

  /** Describes the native registration after the run loop takes this task. */
  virtual void prepare(kqueue_helper& helper) noexcept = 0;

  /** Selects error completion when preparation or registration fails. */
  virtual void complete_submit_error(int result) noexcept = 0;

  /** Selects set_stopped completion when io_context::stop() aborts inflight
   * I/O. */
  virtual void complete_submit_stopped() noexcept = 0;

  /**
   * Returns whether this operation owns the syscall performed after a
   * readiness notification.
   *
   * Objectized layer-2 read/write requests override this hook. The context
   * itself never issues their native I/O calls.
   */
  [[nodiscard]] virtual bool owns_io_step() const noexcept { return false; }

  /** Performs one bounded nonblocking syscall after readiness. */
  [[nodiscard]] virtual int perform_io() noexcept { return 0; }
};

inline void kqueue_task_queue_state::push_cpu(
    kqueue_operation_base& operation) noexcept {
  kqueue_operation_base* head = cpu_head.load(std::memory_order_relaxed);
  do {
    operation.next = head;
  } while (!cpu_head.compare_exchange_weak(
      head, &operation, std::memory_order_release, std::memory_order_relaxed));
}

inline void kqueue_task_queue_state::push_io(
    kqueue_io_operation_base& operation) noexcept {
  kqueue_io_operation_base* head = io_head.load(std::memory_order_relaxed);
  do {
    operation.io_next = head;
  } while (!io_head.compare_exchange_weak(
      head, &operation, std::memory_order_release, std::memory_order_relaxed));
}

inline kqueue_operation_base* kqueue_task_queue_state::pop_cpu_all() noexcept {
  return cpu_head.exchange(nullptr, std::memory_order_acquire);
}

inline kqueue_io_operation_base*
kqueue_task_queue_state::pop_io_all() noexcept {
  return io_head.exchange(nullptr, std::memory_order_acquire);
}

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPERATION_BASE_H_
