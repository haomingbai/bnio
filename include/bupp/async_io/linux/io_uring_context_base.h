#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_H_

#include <bupp/async_io/time.h>
#include <bupp/base/linux/ring.h>
#include <bupp/base/linux/submission_queue_entry.h>
#include <bupp/export.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <linux/io_uring.h>
#include <mutex>
#include <utility>

namespace bupp::async_io::linux_native {

/**
 * Options used to initialize an io_uring-backed async I/O context.
 */
struct io_uring_context_options {
  /**
   * Number of submission queue entries requested for the ring.
   *
   * @see io_uring_queue_init
   */
  unsigned entries = 256;

  /**
   * io_uring setup flags passed to the kernel.
   *
   * Defaults to COOP_TASKRUN (supported since Linux 5.19) — lets the kernel
   * defer task_work, reducing involuntary context switches.
   *
   * Set IORING_SETUP_SINGLE_ISSUER explicitly when you guarantee that all
   * submissions come from a single thread (benchmarks, dedicated event-loop
   * threads).  Do NOT set it when multiple threads may call post() or
   * timer::cancel() / timer::expires_at() — those paths submit SQEs that
   * would race with the run thread.
   *
   * The library falls back automatically on EINVAL for kernels < 5.19.
   *
   * @see io_uring_queue_init
   * @see docs/design/io_uring-setup.md
   */
  unsigned setup_flags = IORING_SETUP_COOP_TASKRUN;

  /**
   * Maximum number of ready CQEs collected in one batch.
   */
  unsigned cqe_batch_window = 64;

  /**
   * Number of non-blocking polling rounds before a run loop parks.
   */
  unsigned wait_spin_count = 4;

  /**
   * Maximum number of CQE completions kept on the local run queue.
   */
  unsigned cqe_inline_completion_threshold = 8;

  /**
   * When true, adds IORING_SETUP_SQPOLL to setup_flags.
   *
   * A kernel thread polls the SQ ring continuously, eliminating
   * io_uring_enter syscalls for submission.  The trade-off is one
   * dedicated CPU core consumed by the kernel poller thread.
   *
   * @see docs/design/io_uring-setup.md
   */
  bool enable_sqpoll = false;

  /**
   * CPU affinity hint for the SQPOLL kernel thread.
   *
   * 0 means no preference.
   *
   * @see io_uring_params::sq_thread_cpu
   */
  unsigned sqpoll_thread_cpu = 0;

  /**
   * SQPOLL idle timeout in milliseconds before the kernel thread parks.
   *
   * @see io_uring_params::sq_thread_idle
   */
  unsigned sqpoll_idle_ms = 1000;
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

/**
 * Run loop and submission context backed by a Linux io_uring instance.
 */
class BUPP_EXPORT io_uring_context {
 public:
  /**
   * Monotonic clock used by async I/O scheduling.
   */
  using steady_clock = bupp::async_io::steady_clock;

  /**
   * Clock used as the default async I/O scheduling clock.
   */
  using clock = bupp::async_io::clock;

  /**
   * Wall-clock type for APIs that explicitly need system time.
   */
  using system_clock = bupp::async_io::system_clock;

  /**
   * Canonical async I/O duration.
   */
  using duration = bupp::async_io::duration;

  /**
   * Time point represented with the default async I/O clock.
   */
  using time_point = bupp::async_io::time_point;

  /**
   * System-clock time point represented with async I/O duration precision.
   */
  using system_time_point = bupp::async_io::system_time_point;

  /**
   * Creates a closed context.
   */
  io_uring_context() noexcept;

  /**
   * Creates a context and attempts to initialize its ring.
   */
  explicit io_uring_context(const io_uring_context_options& options) noexcept;

  /**
   * Stops and releases the context ring.
   */
  ~io_uring_context() noexcept;

  /**
   * Copy construction is disabled because the context owns an io_uring ring.
   */
  io_uring_context(const io_uring_context&) = delete;

  /**
   * Copy assignment is disabled because the context owns an io_uring ring.
   */
  io_uring_context& operator=(const io_uring_context&) = delete;

  /**
   * Move construction is disabled because the context owns synchronization
   * primitives and thread-local run-loop state.
   */
  io_uring_context(io_uring_context&&) = delete;

  /**
   * Move assignment is disabled because the context owns synchronization
   * primitives and thread-local run-loop state.
   */
  io_uring_context& operator=(io_uring_context&&) = delete;

  /**
   * Initializes the context ring with the supplied options.
   *
   * @see io_uring_queue_init
   */
  int queue_init(const io_uring_context_options& options) noexcept;

  /**
   * Releases the context ring and wakes any waiting run loop.
   *
   * @see io_uring_queue_exit
   */
  void queue_exit() noexcept;

  /**
   * Returns whether the context currently owns an open ring.
   */
  [[nodiscard]] bool is_open() const noexcept;

  /**
   * Prepares an operation SQE without submitting the ring.
   */
  template <class Operation>
  int prepare(Operation& operation) noexcept {
    assert_running();
    auto lock = lock_uring();
    return prepare_locked(operation);
  }

  /**
   * Submits all prepared SQEs on the context ring.
   *
   * @see io_uring_submit
   */
  int submit() noexcept;

  /**
   * Prepares one operation and submits the context ring.
   */
  template <class Operation>
  int submit(Operation& operation) noexcept {
    assert_running();

    int submit_result = 0;
    {
      auto lock = lock_uring();
      const int prepare_result = prepare_locked(operation);
      if (prepare_result < 0) {
        return prepare_result;
      }
      submit_result = submit_locked();
    }

    if (submit_result >= 0) {
      notify_waiters();
    }
    return submit_result;
  }

  /**
   * Runs a batch of submission work while holding the uring mutex once.
   *
   * The callback receives two callables: prepare(operation) reserves and fills
   * one SQE, and submit() submits all SQEs prepared so far.
   */
  template <class Function>
  void submit_batch(Function&& function) noexcept {
    assert_running();

    bool should_notify = false;
    {
      auto lock = lock_uring();
      auto prepare = [this](auto& operation) noexcept {
        return prepare_locked(operation);
      };
      auto submit = [this, &should_notify]() noexcept {
        const int result = submit_locked();
        if (result >= 0) {
          should_notify = true;
        }
        return result;
      };

      std::forward<Function>(function)(prepare, submit);
    }

    if (should_notify) {
      notify_waiters();
    }
  }

  /**
   * Posts an operation for execution by the context run loop.
   */
  int post(io_uring_operation_base& operation) noexcept;

  /**
   * Runs queued tasks and CQE completions until the context finishes.
   */
  void run() noexcept;

  /**
   * Requests the run loop to finish.
   */
  int stop() noexcept;

  /**
   * Returns whether the current thread is running this context.
   */
  [[nodiscard]] bool is_in_context() const noexcept;

  /**
   * Returns the kernel io_uring feature flags reported at ring creation.
   *
   * Use IORING_FEAT_* macros to test individual capabilities, e.g.:
   *   if (ctx.kernel_features() & IORING_FEAT_RECVSEND_BUNDLE) { ... }
   *
   * @see io_uring_params::features
   */
  [[nodiscard]] unsigned kernel_features() const noexcept {
    return kernel_features_;
  }

 private:
  /**
   * Lifecycle state for the context run loop.
   */
  enum class context_state {
    running,
    finishing,
    finished,
  };

  /**
   * Next action selected by the run loop.
   */
  enum class run_phase {
    run_ready_tasks,
    wait_for_work,
    finish_drain,
    finished,
  };

  /**
   * Small copy of CQE data used after the CQ head advances.
   */
  struct cqe_data;

  // Per-run-scope task stack. This is intentionally not atomic: only the
  // owning run() thread pushes into it and drains it with pop_all. Cross-thread
  // producers must publish through global_tasks_ instead.
  /**
   * Intrusive stack of operations used by run-loop phases.
   */
  struct operation_queue;

  /**
   * Acquires the uring mutex (or skips it when SINGLE_ISSUER is active).
   *
   * Returns a unique_lock that may be in an unlocked state when the kernel
   * guarantees single-issuer semantics.
   */
  [[nodiscard]] std::unique_lock<std::mutex> lock_uring() const noexcept {
    std::unique_lock lock(uring_mutex_, std::defer_lock);
    if (!single_issuer_) {
      lock.lock();
    }
    return lock;
  }

  /**
   * Initialises the ring with the supplied flags, retrying without
   * SINGLE_ISSUER / COOP_TASKRUN / SQPOLL on EINVAL.
   *
   * @return 0 on success, or a negative errno.
   */
  int init_ring_params(unsigned entries, unsigned flags,
                       bupp::base::params& queue_params) noexcept;

  /**
   * Submits prepared SQEs while the uring mutex is already held.
   */
  int submit_locked() noexcept;

  /**
   * Verifies in debug builds that the context is running.
   */
  void assert_running() const noexcept;

  /**
   * Runs ready local and global tasks.
   */
  [[nodiscard]] run_phase handle_run_ready_tasks(
      operation_queue& local_tasks) noexcept;

  /**
   * Waits for work when no tasks are immediately ready.
   */
  [[nodiscard]] run_phase handle_wait_for_work(
      operation_queue& local_tasks) noexcept;

  /**
   * Drains remaining work during shutdown.
   */
  [[nodiscard]] run_phase handle_finish_drain(
      operation_queue& local_tasks) noexcept;

  /**
   * Polls briefly for CQEs or posted tasks before blocking.
   */
  [[nodiscard]] run_phase spin_for_work(operation_queue& local_tasks) noexcept;

  /**
   * Waits on the condition variable while another thread waits for io_uring.
   */
  [[nodiscard]] run_phase wait_for_condition_work(
      operation_queue& local_tasks) noexcept;

  /**
   * Waits for io_uring completion events.
   */
  [[nodiscard]] run_phase wait_for_io_work(
      operation_queue& local_tasks) noexcept;

  /**
   * Publishes a single operation to the global task queue.
   */
  void push_global_task(io_uring_operation_base& operation) noexcept;

  /**
   * Publishes all operations from a local queue to the global task queue.
   */
  void push_global_tasks(operation_queue& operations) noexcept;

  /**
   * Moves globally published tasks into a local queue.
   */
  [[nodiscard]] bool move_global_tasks(operation_queue& local_tasks) noexcept;

  /**
   * Wakes threads waiting for context work.
   */
  void notify_waiters() noexcept;

  /**
   * Submits a wake-up SQE for an active io_uring waiter.
   */
  int submit_wake_task() noexcept;

  /**
   * Submits a wake-up SQE while the uring mutex is already held.
   */
  int submit_wake_task_locked() noexcept;

  /**
   * Returns the sentinel user data used for wake-up SQEs.
   */
  [[nodiscard]] static void* wake_user_data() noexcept;

  /**
   * Waits for at least one CQE event on the native ring descriptor.
   */
  [[nodiscard]] int wait_for_cqe_event() noexcept;

  /**
   * Collects and dispatches ready CQE-backed tasks.
   */
  [[nodiscard]] bool collect_ready_cqes(operation_queue& local_tasks) noexcept;

  /**
   * Collects ready CQEs into an operation queue.
   */
  [[nodiscard]] unsigned collect_cqe_tasks(operation_queue& cqe_tasks) noexcept;

  /**
   * Dispatches collected CQE tasks locally or through the global queue.
   */
  void dispatch_cqe_tasks(operation_queue& cqe_tasks, unsigned task_count,
                          operation_queue& local_tasks) noexcept;

  /**
   * Enqueues the operation represented by one CQE data record.
   */
  [[nodiscard]] bool enqueue_cqe_task(const cqe_data& data,
                                      operation_queue& tasks) noexcept;

  /**
   * Returns whether the context should leave the running state.
   */
  [[nodiscard]] bool should_finish() const noexcept;

  /**
   * Drains work and marks the context finished.
   */
  void finish(operation_queue& local_tasks) noexcept;

  bupp::base::ring ring_;
  mutable std::mutex uring_mutex_;
  bool single_issuer_ = false;
  unsigned kernel_features_ = 0;

  /**
   * Prepares an operation while the uring mutex is already held.
   */
  template <class Operation>
  int prepare_locked(Operation& operation) noexcept {
    if (!ring_.is_open()) {
      return -EINVAL;
    }

    bupp::base::submission_queue_entry sqe = ring_.get_sqe();
    if (sqe.raw() == nullptr) {
      return -EAGAIN;
    }

    operation.prepare(sqe);
    sqe.set_data(static_cast<io_uring_operation_base*>(&operation));
    return 0;
  }
  // Cross-thread task publication stack. run() workers drain it in one batch
  // and reverse the batch before moving work into their local operation_queue.
  std::atomic<io_uring_operation_base*> global_tasks_{nullptr};
  std::mutex wait_mutex_;
  std::condition_variable wait_cv_;
  std::atomic<context_state> state_{context_state::finished};
  std::atomic_bool io_waiter_active_{false};
  bool queue_initialized_ = false;
  bool wake_task_pending_ = false;
  unsigned cqe_batch_window_ = io_uring_context_options{}.cqe_batch_window;
  unsigned wait_spin_count_ = io_uring_context_options{}.wait_spin_count;
  unsigned cqe_inline_completion_threshold_ =
      io_uring_context_options{}.cqe_inline_completion_threshold;

  static thread_local io_uring_context* current_context_;
  static thread_local operation_queue* current_local_tasks_;
};

}  // namespace bupp::async_io::linux_native

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_H_
