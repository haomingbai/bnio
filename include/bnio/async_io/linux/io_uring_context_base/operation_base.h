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
#include <mutex>

namespace bnio::base {
class submission_queue_entry;
}

namespace bnio::async_io::linux_native {

class io_uring_operation_base;
class io_uring_io_operation_base;

/** Per-worker local CPU queue and wake channel.
 *
 * Each worker is linked into exactly one of the shared state's two
 * intrusive doubly-linked lists at a time: the run list while it is
 * processing work, or the suspend list while it sleeps in the native
 * poller.  A worker moves itself between the lists under the
 * corresponding list lock when it begins/ends a wait; remote threads
 * only touch a node while holding that node's list lock, which also
 * keeps the owning worker from being destroyed (UAF protection).
 */
struct BNIO_EXPORT io_uring_local_task_queue_state {
  void push_cpu(io_uring_operation_base& operation) noexcept;

  /** Pushes a linked list of tasks (order preserved relative to the caller's
   *  reversed FIFO). */
  void push_cpu(io_uring_operation_base* operations) noexcept;

  /** Removes the whole CPU queue in bulk; used by fetch and stealing. */
  [[nodiscard]] io_uring_operation_base* pop_cpu_all() noexcept;

  std::atomic<io_uring_operation_base*> cpu_head{nullptr};

  /** Doubly-linked list links for the shared run/suspend lists. */
  io_uring_local_task_queue_state* prev = nullptr;
  io_uring_local_task_queue_state* next = nullptr;

  /** Per-worker wake channel for directed wakeups. */
  bnio::base::wake_channel wake_channel_;
};

/** One doubly-linked list of worker local states guarded by its own lock.
 *
 * The list head and its guard lock are kept together so every operation on
 * the list takes exactly the lock that owns it; a worker resides in at most
 * one of the two lists at any time. */
struct BNIO_EXPORT io_uring_worker_state_list {
  /** Guards the list and the lifetime of its nodes. */
  std::mutex lock;
  /** Head of the intrusive list. */
  io_uring_local_task_queue_state* head = nullptr;
  /** Round-robin cursor; points into the list. Only the suspend list uses
   *  it (for wake_one_sleeping()). */
  io_uring_local_task_queue_state* cursor = nullptr;
};

/** Worker-local-state registry split into running and suspended lists. */
struct BNIO_EXPORT io_uring_worker_state_registry {
  /** Workers currently processing work (steal targets). */
  io_uring_worker_state_list run;
  /** Workers sleeping in the native poller (directed-wake targets). */
  io_uring_worker_state_list suspend;
};

/** Shared MPSC CPU/I/O queues and worker-group lifecycle state. */
struct BNIO_EXPORT io_uring_task_queue_state {
  using try_fetch_timeout_fn = bool (*)(void*, async_io::time_point&,
                                        io_uring_operation_base*&) noexcept;

  void push_cpu(io_uring_operation_base& operation) noexcept;

  [[nodiscard]] io_uring_operation_base* pop_cpu_all() noexcept;

  void push_io(io_uring_io_operation_base& operation) noexcept;

  [[nodiscard]] io_uring_io_operation_base* pop_io_all() noexcept;

  /**
   * Wakes exactly one sleeping worker by writing its per-worker wake
   * channel. Takes only the suspend list's lock: a node on the suspend
   * list is alive (the owner unregisters under the same lock before its
   * context is destroyed), so the write cannot race a close.
   *
   * @return true if a worker was woken; false if nobody is sleeping.
   */
  [[nodiscard]] bool wake_one_sleeping() noexcept;

  std::atomic<io_uring_operation_base*> cpu_head{nullptr};
  std::atomic<io_uring_io_operation_base*> io_head{nullptr};
  std::atomic<std::size_t> awake_workers{0};

  /** Running/suspended worker local states, each list guarded by its own
   *  lock. */
  io_uring_worker_state_registry workers;

  std::atomic<int> life_state{0};  // 0 = running, 1 = stopping

  /** Opaque shared lazy timer heap and its non-blocking fetch entry point. */
  void* timeout_heap = nullptr;
  try_fetch_timeout_fn try_fetch_timeout_operations = nullptr;

  /** Shared wake channel owned by io_context.
   *
   * io_context creates the channel once and writes to it to wake
   * workers during shutdown.  Each worker's io_uring_context registers
   * read interest (IORING_POLL_ADD) before sleeping.  In normal
   * operation a single worker is woken directly via its per-worker
   * channel (see wake_one_sleeping()); this shared channel remains the
   * broadcast path used by stop() and as a fallback.
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

  /** Selects set_stopped completion when io_context::stop() aborts inflight
   * I/O. */
  virtual void complete_submit_stopped() noexcept = 0;

  /** Intrusive links for the inflight doubly-linked list. */
  io_uring_io_operation_base* io_next = nullptr;
  io_uring_io_operation_base* io_prev = nullptr;
};

inline void io_uring_local_task_queue_state::push_cpu(
    io_uring_operation_base& operation) noexcept {
  io_uring_operation_base* head = cpu_head.load(std::memory_order_relaxed);
  do {
    operation.next = head;
  } while (!cpu_head.compare_exchange_weak(
      head, &operation, std::memory_order_release, std::memory_order_relaxed));
}

inline void io_uring_local_task_queue_state::push_cpu(
    io_uring_operation_base* operations) noexcept {
  while (operations != nullptr) {
    io_uring_operation_base* operation = operations;
    operations = operations->next;
    operation->next = nullptr;
    push_cpu(*operation);
  }
}

inline io_uring_operation_base*
io_uring_local_task_queue_state::pop_cpu_all() noexcept {
  return cpu_head.exchange(nullptr, std::memory_order_acquire);
}

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

/** Links a local state at the head of a worker list. Caller holds the
 *  list's lock. */
inline void io_uring_link_local_state(
    io_uring_worker_state_list& list,
    io_uring_local_task_queue_state* node) noexcept {
  node->prev = nullptr;
  node->next = list.head;
  if (list.head != nullptr) {
    list.head->prev = node;
  }
  list.head = node;
}

/** Unlinks a local state from its worker list. Caller holds the list's
 *  lock. */
inline void io_uring_unlink_local_state(
    io_uring_worker_state_list& list,
    io_uring_local_task_queue_state* node) noexcept {
  if (node->prev != nullptr) {
    node->prev->next = node->next;
  } else {
    list.head = node->next;
  }
  if (node->next != nullptr) {
    node->next->prev = node->prev;
  }
  node->prev = nullptr;
  node->next = nullptr;
}

/** Returns whether the node is currently linked into the given list.
 *  Caller holds the list's lock. */
inline bool io_uring_local_state_in_list(
    const io_uring_worker_state_list& list,
    const io_uring_local_task_queue_state* node) noexcept {
  return node->prev != nullptr || list.head == node;
}

inline bool io_uring_task_queue_state::wake_one_sleeping() noexcept {
  io_uring_worker_state_list& suspend = workers.suspend;
  std::lock_guard<std::mutex> guard(suspend.lock);
  // Validate the saved cursor is still in the list; otherwise restart from
  // the head. The cursor advances so repeated wake-ups rotate fairly.
  io_uring_local_task_queue_state* start = suspend.head;
  if (suspend.cursor != nullptr) {
    io_uring_local_task_queue_state* scan = start;
    while (scan != nullptr && scan != suspend.cursor) {
      scan = scan->next;
    }
    if (scan != nullptr) {
      start = suspend.cursor;
    } else {
      suspend.cursor = nullptr;
    }
  }
  if (start == nullptr) {
    return false;
  }
  (void)start->wake_channel_.wake();
  suspend.cursor = start->next;
  return true;
}

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPERATION_BASE_H_
