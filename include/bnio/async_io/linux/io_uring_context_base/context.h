/**
 * @file context.h
 * @brief io_uring_context class declaration.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_CONTEXT_H_
#define BNIO_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_CONTEXT_H_

#include <bnio/async_io/descriptor_view.h>
#include <bnio/async_io/dns.h>
#include <bnio/async_io/linux/io_uring_context_base/operation_base.h>
#include <bnio/async_io/linux/io_uring_context_base/options.h>
#include <bnio/async_io/time.h>
#include <bnio/base/linux/ring.h>
#include <bnio/export.h>

#include <atomic>
#include <cstdint>
#include <string_view>

namespace bnio::async_io::linux_native {

/**
 * Single-threaded run loop backed by a Linux io_uring instance.
 */
class BNIO_EXPORT io_uring_context {
 public:
  /** Monotonic clock used by async I/O scheduling. */
  using steady_clock = bnio::async_io::steady_clock;

  /** Clock used as the default async I/O scheduling clock. */
  using clock = bnio::async_io::clock;

  /** Wall-clock type for APIs that explicitly need system time. */
  using system_clock = bnio::async_io::system_clock;

  /** Canonical async I/O duration. */
  using duration = bnio::async_io::duration;

  /** Time point represented with the default async I/O clock. */
  using time_point = bnio::async_io::time_point;

  /** System-clock time point with async I/O duration precision. */
  using system_time_point = bnio::async_io::system_time_point;

  /** Creates a closed context. */
  io_uring_context() noexcept;

  /** Creates a context and attempts to initialize its ring. */
  explicit io_uring_context(const io_uring_context_options& options) noexcept;

  /** Stops and releases the context ring. */
  ~io_uring_context() noexcept;

  /** Disabled: the context owns an io_uring ring. */
  io_uring_context(const io_uring_context&) = delete;

  /** Disabled: the context owns an io_uring ring. */
  io_uring_context& operator=(const io_uring_context&) = delete;

  /** Disabled: the context owns a ring and thread-local run-loop state. */
  io_uring_context(io_uring_context&&) = delete;

  /** Disabled: the context owns a ring and thread-local run-loop state. */
  io_uring_context& operator=(io_uring_context&&) = delete;

  /** Initializes the ring with the supplied options. */
  int queue_init(const io_uring_context_options& options) noexcept;

  /** Releases the ring and wakes any active run loop. */
  void queue_exit() noexcept;

  /** Returns whether the context currently owns an open ring. */
  [[nodiscard]] bool is_open() const noexcept;

  /** Kernel io_uring feature flags reported at ring creation; test with
   *  IORING_FEAT_* macros. See io_uring_params::features. */
  [[nodiscard]] unsigned kernel_features() const noexcept {
    return kernel_features_;
  }

  /** Creates a sender that waits for events on a file descriptor. */
  [[nodiscard]] auto async_poll(bnio::async_io::descriptor_view descriptor,
                                unsigned poll_mask);

  /** Sender resolving a DNS query into caller-provided result storage on
   *  the context run loop. */
  [[nodiscard]] auto async_resolve(bnio::async_io::dns_query query,
                                   bnio::async_io::dns_result_view result);

  /** Sender resolving a host and service into caller-provided result storage
   *  on the context run loop. */
  [[nodiscard]] auto async_resolve(std::string_view host,
                                   std::string_view service,
                                   bnio::async_io::dns_result_view result);

  /** Runs queued tasks and CQE completions until the context finishes.
   *
   *  Calling run() again after it has finished, or racing it past its
   *  lifetime, is unsupported. */
  void run() noexcept;

  /** Requests the run loop to finish. */
  int stop() noexcept;

  /** Returns whether the current thread is running this context. */
  [[nodiscard]] bool is_in_context() const noexcept;

  /** Selects externally owned shared state for a worker group.
   *
   *  Not thread-safe; call before publishing work or run(). The state must
   *  stay valid until this context stops running. */
  void set_global_state(io_uring_task_queue_state* state) noexcept;

  /** Shared task queue state bound to this worker, or null before run(). */
  [[nodiscard]] io_uring_task_queue_state* get_global_state() const noexcept {
    return global_state_;
  }

  /** Wakes the run loop through the context eventfd. */
  void notify_one_waiter() noexcept;

  /** Returns whether this run-loop worker has published a sleeping state. */
  [[nodiscard]] bool is_waiting() const noexcept;

  /** Negative errno that aborted enter_run(), or 0 if run() entered normally
   *  (or never ran).
   *
   *  Written once by the run() caller before any worker starts and read after
   *  run() returns, so no synchronization is needed; run() surfaces it as its
   *  error_code. */
  [[nodiscard]] int enter_run_error() const noexcept {
    return enter_run_error_;
  }

  /** Internal: posts an operation for execution by the run loop.
   *
   *  Always enqueues and never refuses work; this layer does not gate on
   *  life_state. Direct callers must keep the context running until the
   *  operation reaches a terminal receiver call. */
  int post(io_uring_operation_base& operation) noexcept;

  /** Internal: publishes I/O for passive preparation by the run loop.
   *
   *  Always enqueues and never refuses work; this layer does not gate on
   *  life_state. Direct callers must keep the context running until the
   *  operation reaches a terminal receiver call. */
  void publish_io(io_uring_io_operation_base& operation) noexcept;

  /** Worker-local task queue state.
   *
   *  Linked into the shared suspend list while the worker sleeps; its queues
   *  are worker-exclusive. */
  [[nodiscard]] io_uring_local_task_queue_state* local_state() noexcept {
    return &local_state_;
  }

  /** Fetches a batch of CPU tasks: local queue first, then shared, stopping
   *  at the first source that yields work. */
  [[nodiscard]] io_uring_operation_base* fetch_cpu_task() noexcept;

  /** Fetches and executes a batch of CPU tasks. Returns true if work ran. */
  [[nodiscard]] bool run_cpu_batch() noexcept;

 private:
  struct operation_queue {
    void push(io_uring_operation_base& operation) noexcept;

    void push(io_uring_operation_base* operations) noexcept;

    [[nodiscard]] io_uring_operation_base* pop_all() noexcept;

    io_uring_operation_base* head = nullptr;
  };

  /** Unregisters this worker's local state from the shared list. */
  void unregister_local_state() noexcept;

  int prepare_io(io_uring_io_operation_base& operation) noexcept;

  int submit_ring() noexcept;

  /** Enables a disabled ring on its run-loop thread. */
  int enable_ring() noexcept;

  /** Small copy of CQE data used after the CQ head advances. */
  struct cqe_data;

  /** CQE consumer for collect_cqe_tasks(): classifies each CQE and enqueues
   *  operation CQEs into the caller's queue, counting them. */
  struct cqe_collector;

  /** Lifecycle state for the context run loop. */
  enum class context_state {
    running,
    finishing,
    finished,
  };

  /** Next action selected by the run loop. */
  enum class run_phase {
    run_ready_tasks,
    wait_for_work,
    finish_drain,
    finished,
  };

  /** Publishes a local queue's operations to the shared CPU queue. */
  void push_cpu_tasks(operation_queue& operations) noexcept;

  /** Consumes the SQ-full retry slot, then the local queue, then the shared
   *  queue; submits once. Stops pulling new sources as soon as the SQ fills
   *  so backpressured work stays queued. */
  [[nodiscard]] bool consume_io_tasks() noexcept;

  /** Prepares a list of operations into SQEs, appending successes to
   *  @p prepared (LIFO) and failing non-EAGAIN prepare errors inline.
   *
   *  @return nullptr when fully consumed, else the remainder headed by the
   *          operation that hit a full SQ (-EAGAIN). */
  [[nodiscard]] io_uring_io_operation_base* prepare_io_batch(
      io_uring_io_operation_base* operations,
      io_uring_io_operation_base*& prepared) noexcept;

  /** Registers every prepared operation in the inflight list and clears its
   *  next pointer. Must cover every operation holding a prepared SQE,
   *  including -EAGAIN re-submissions. */
  void track_prepared_batch(io_uring_io_operation_base*& prepared) noexcept;

  /** Submits the prepared SQEs, which must already be registered inflight: a
   *  submit failure then fails them through fail_io_list(). Returns 0 without
   *  a syscall when @p prepared is null. */
  [[nodiscard]] int submit_and_track_prepared(
      io_uring_io_operation_base* prepared) noexcept;

  /** Unregisters a list from the inflight list, fails each operation with
   *  @p result, and pushes it to the local CPU queue. */
  void fail_io_list(io_uring_io_operation_base* head, int result) noexcept;

  /** Adds an I/O operation to the inflight doubly-linked list. */
  void add_inflight(io_uring_io_operation_base& operation) noexcept;

  /** Removes an I/O operation from the inflight doubly-linked list. */
  void remove_inflight(io_uring_io_operation_base& operation) noexcept;

  /** Aborts all inflight I/O operations during shutdown. */
  void abort_inflight_io() noexcept;

  /** Completes a linked list of unregistered I/O operations as stopped and
   *  pushes them to the local CPU queue. */
  void drain_io_list_complete_stopped(
      io_uring_io_operation_base* head) noexcept;

  /** Moves due passive-timer completions into the local CPU queue. */
  [[nodiscard]] bool consume_timeout_operations() noexcept;

  void begin_wait() noexcept;

  void end_wait() noexcept;

  /** Signals the eventfd used by posted work and stop requests. */
  [[nodiscard]] int signal_eventfd() noexcept;

  /** Drains pending eventfd notifications. */
  void drain_eventfd() noexcept;

  /**
   * Arms the IORING_POLL_ADD for fd; -EAGAIN is re-armed on the next pass.
   * pending_flag is set while armed, cleared when the CQE is collected.
   * Returns 1 when armed or already pending, 0 when stopping (do not block
   * without an independent wake source), or a negative errno (-EINVAL closed,
   * others fatal for the channel).
   */
  [[nodiscard]] int arm_wake_poll(int fd, void* user_data,
                                  bool& pending_flag) noexcept;

  /** Sentinel user data for eventfd poll SQEs. */
  [[nodiscard]] static void* eventfd_user_data() noexcept;

  /** Sentinel user data for local wake channel poll SQEs. */
  [[nodiscard]] static void* local_eventfd_user_data() noexcept;

  /** Runs ready local and posted tasks. */
  [[nodiscard]] run_phase handle_run_ready_tasks() noexcept;

  /** Waits for work when no tasks are immediately ready. */
  [[nodiscard]] run_phase handle_wait_for_work() noexcept;

  /** Drains remaining work during shutdown. */
  [[nodiscard]] run_phase handle_finish_drain() noexcept;

  /** Polls briefly for CQEs or posted tasks before blocking. */
  [[nodiscard]] run_phase spin_for_work() noexcept;

  /** Waits for io_uring completion events. */
  [[nodiscard]] run_phase wait_for_io_work() noexcept;

  /**
   * Collects work that became ready without blocking: ready CQEs, one
   * CPU batch, due timers, and queued I/O.
   */
  [[nodiscard]] bool collect_pending_work() noexcept;

  /** Rearms both wake polls before a blocking wait.
   *
   *  @return wait_for_work when the caller may block; run_ready_tasks on
   *          transient -EAGAIN SQ pressure; finish_drain on a fatal re-arm
   *          failure or when stopping.
   */
  [[nodiscard]] run_phase rearm_wake_polls_for_wait() noexcept;

  /** Fetches due timers; if none is ready, fills @p timeout with the nearest
   *  deadline.
   *
   *  @param timeout         Timespec filled when a deadline exists.
   *  @param timeout_pointer Set to @p timeout when a deadline exists, else
   *                         null.
   *  @return wait_for_work to rearm and block; otherwise the phase to enter
   *          because work was found.
   */
  [[nodiscard]] run_phase prepare_wait_timeout(
      __kernel_timespec& timeout, __kernel_timespec*& timeout_pointer) noexcept;

  /** Returns whether the shared state has requested closing. */
  [[nodiscard]] bool closing_requested() const noexcept;

  /** Returns whether stop() has been called on this context. */
  [[nodiscard]] bool stop_requested() const noexcept;

  /** Returns whether the context should leave the running state. */
  [[nodiscard]] bool should_finish() const noexcept;

  /** Drains work and marks the context finished.
   *
   *  Tolerates a broken ring or wake channel: every operation still reaches a
   *  terminal receiver call. */
  void finish() noexcept;

  /** Aborts remaining inflight and queued I/O and delivers every completion,
   *  looping until no work remains. Shared by finish() and queue_exit();
   *  receivers run synchronously on the calling thread. */
  void abort_and_deliver_completions() noexcept;

  /** Waits for at least one CQE event on the native ring descriptor. */
  [[nodiscard]] int wait_for_cqe_event(
      __kernel_timespec* timeout = nullptr) noexcept;

  /** Collects and dispatches ready CQE-backed tasks. */
  [[nodiscard]] bool collect_ready_cqes() noexcept;

  /** Kind of CQE user data, used to dispatch collected CQEs. */
  enum class cqe_user_data_kind {
    eventfd,
    local_eventfd,
    operation,
  };

  /**
   * Classifies a CQE user-data pointer as the shared eventfd sentinel, the
   * per-worker wake channel sentinel, or an operation.
   */
  [[nodiscard]] static cqe_user_data_kind classify_cqe_user_data(
      void* user_data) noexcept;

  /** Collects ready CQEs into an operation queue. */
  [[nodiscard]] unsigned collect_cqe_tasks(operation_queue& cqe_tasks) noexcept;

  /** Dispatches collected CQE tasks locally or via the shared CPU queue. */
  void dispatch_cqe_tasks(operation_queue& cqe_tasks,
                          unsigned task_count) noexcept;

  /** Enqueues the operation represented by one CQE data record. */
  [[nodiscard]] bool enqueue_cqe_task(const cqe_data& data,
                                      operation_queue& tasks) noexcept;

  /** Initializes the ring, retrying without bnio-managed setup flags on
   *  EINVAL. @return 0 or a negative errno. */
  int init_ring_params(unsigned entries, unsigned flags,
                       bnio::base::params& queue_params) noexcept;

  /** Initializes run-loop state; false means the caller must exit early
   *  after restoring current_context_. */
  [[nodiscard]] bool enter_run() noexcept;

  /** Rollback for enter_run() failure: delivers queued operations through
   *  finish()'s abort path, undoes the awake-worker count, clears
   *  run_active. */
  bool fail_enter_run(bool awake_worker_added) noexcept;

  /** Drains CPU tasks, timer expirations, and optionally CQEs until the local
   *  task queue is empty. */
  void drain_local_tasks(bool include_cqe) noexcept;

  /** Applies configuration from options to context member variables. */
  void apply_context_options(const io_uring_context_options& options) noexcept;

  /** Verifies in debug builds that the context is running. */
  void assert_running() const noexcept;

  bnio::base::ring ring_;
  io_uring_context_options options_{};
  unsigned kernel_features_ = 0;

  /** Lifecycle state and run-loop flags for this context. */
  struct run_state {
    std::atomic<context_state> state{context_state::finished};
    std::atomic_bool run_active{false};
    std::atomic_bool waiting{false};
    bool queue_initialized = false;
    bool ring_disabled = false;
  };
  run_state run_state_;

  /** enter_run() failure errno, 0 otherwise; owned by the run() caller
   *  thread. See enter_run_error(). */
  int enter_run_error_ = 0;

  /** Poll-arming state for the shared and per-worker wake channels. */
  struct poll_state {
    bool eventfd_poll_pending = false;
    bool local_eventfd_poll_pending = false;
  };
  poll_state poll_state_;

  static thread_local io_uring_context* current_context_;
  io_uring_task_queue_state* global_state_ = nullptr;
  io_uring_local_task_queue_state local_state_;
  io_uring_io_operation_base* inflight_io_head_ = nullptr;

  /** Unsubmitted remainder of the current I/O batch, set when the SQ fills.
   *  Owned by the run-loop thread and retried before any queue is popped;
   *  abort_inflight_io() drains it like the I/O queues. */
  io_uring_io_operation_base* pending_io_retry_ = nullptr;

  /** Run-loop scheduling budget. */
  struct scheduling_state {
    unsigned local_task_budget = 0;
  };
  scheduling_state scheduling_state_;
};

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_CONTEXT_H_
