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
  /**
   * Monotonic clock used by async I/O scheduling.
   */
  using steady_clock = bnio::async_io::steady_clock;

  /**
   * Clock used as the default async I/O scheduling clock.
   */
  using clock = bnio::async_io::clock;

  /**
   * Wall-clock type for APIs that explicitly need system time.
   */
  using system_clock = bnio::async_io::system_clock;

  /**
   * Canonical async I/O duration.
   */
  using duration = bnio::async_io::duration;

  /**
   * Time point represented with the default async I/O clock.
   */
  using time_point = bnio::async_io::time_point;

  /**
   * System-clock time point represented with async I/O duration precision.
   */
  using system_time_point = bnio::async_io::system_time_point;

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
   * Move construction is disabled because the context owns a ring and
   * thread-local run-loop state.
   */
  io_uring_context(io_uring_context&&) = delete;

  /**
   * Move assignment is disabled because the context owns a ring and
   * thread-local run-loop state.
   */
  io_uring_context& operator=(io_uring_context&&) = delete;

  /**
   * Initializes the context ring with the supplied options.
   *
   * @see io_uring_queue_init
   */
  int queue_init(const io_uring_context_options& options) noexcept;

  /**
   * Releases the context ring and wakes any active run loop.
   *
   * @see io_uring_queue_exit
   */
  void queue_exit() noexcept;

  /**
   * Returns whether the context currently owns an open ring.
   */
  [[nodiscard]] bool is_open() const noexcept;

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

  /**
   * Creates a sender that waits for events on a file descriptor.
   */
  [[nodiscard]] auto async_poll(bnio::async_io::descriptor_view descriptor,
                                unsigned poll_mask);

  /**
   * Creates a sender that resolves a DNS query into caller-provided result
   * storage on the context run loop.
   */
  [[nodiscard]] auto async_resolve(bnio::async_io::dns_query query,
                                   bnio::async_io::dns_result_view result);

  /**
   * Creates a sender that resolves a host and service into caller-provided
   * result storage on the context run loop.
   */
  [[nodiscard]] auto async_resolve(std::string_view host,
                                   std::string_view service,
                                   bnio::async_io::dns_result_view result);

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
   * Selects externally owned shared state for a worker group.
   *
   * This function is not thread-safe and must be called before publishing
   * work or calling run(). The state must remain valid until this context
   * stops running.
   */
  void set_global_state(io_uring_task_queue_state* state) noexcept;

  /**
   * Wakes the run loop through the context eventfd.
   */
  void notify_one_waiter() noexcept;

  /** Returns whether this run-loop worker has published a sleeping state. */
  [[nodiscard]] bool is_waiting() const noexcept;

  /**
   * Posts an operation for execution by the context run loop.
   */
  int post(io_uring_operation_base& operation) noexcept;

  /** Publishes I/O for passive preparation by the context run loop. */
  void publish_io(io_uring_io_operation_base& operation) noexcept;

 private:
  struct operation_queue {
    void push(io_uring_operation_base& operation) noexcept;

    void push(io_uring_operation_base* operations) noexcept;

    [[nodiscard]] io_uring_operation_base* pop_all() noexcept;

    io_uring_operation_base* head = nullptr;
  };

  int prepare_io(io_uring_io_operation_base& operation) noexcept;

  int submit_ring() noexcept;

  /** Enables a disabled ring on its run-loop thread. */
  int enable_ring() noexcept;

  /**
   * Small copy of CQE data used after the CQ head advances.
   */
  struct cqe_data;

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
   * Publishes all operations from a local queue to the shared CPU-task queue.
   */
  void push_cpu_tasks(operation_queue& operations) noexcept;

  /** Moves shared CPU tasks into the run-loop-local queue. */
  [[nodiscard]] bool move_cpu_tasks() noexcept;

  /** Consumes ring-local control I/O and all shared I/O after CPU work. */
  [[nodiscard]] bool consume_io_tasks() noexcept;

  /** Adds an I/O operation to the inflight doubly-linked list. */
  void add_inflight(io_uring_io_operation_base& operation) noexcept;

  /** Removes an I/O operation from the inflight doubly-linked list. */
  void remove_inflight(io_uring_io_operation_base& operation) noexcept;

  /** Aborts all inflight I/O operations during shutdown. */
  void abort_inflight_io() noexcept;

  /** Moves due passive-timer completions into the local CPU queue. */
  [[nodiscard]] bool consume_timeout_operations() noexcept;

  void begin_wait() noexcept;

  void end_wait() noexcept;

  /**
   * Signals the eventfd used by posted work and stop requests.
   */
  [[nodiscard]] int signal_eventfd() noexcept;

  /**
   * Drains pending eventfd notifications.
   */
  void drain_eventfd() noexcept;

  /**
   * Submits the internal eventfd poll request.
   */
  [[nodiscard]] int submit_eventfd_poll() noexcept;

  /**
   * Returns the sentinel user data used for eventfd poll SQEs.
   */
  [[nodiscard]] static void* eventfd_user_data() noexcept;

  /**
   * Runs ready local and posted tasks.
   */
  [[nodiscard]] run_phase handle_run_ready_tasks() noexcept;

  /**
   * Waits for work when no tasks are immediately ready.
   */
  [[nodiscard]] run_phase handle_wait_for_work() noexcept;

  /**
   * Drains remaining work during shutdown.
   */
  [[nodiscard]] run_phase handle_finish_drain() noexcept;

  /**
   * Polls briefly for CQEs or posted tasks before blocking.
   */
  [[nodiscard]] run_phase spin_for_work() noexcept;

  /**
   * Waits for io_uring completion events.
   */
  [[nodiscard]] run_phase wait_for_io_work() noexcept;

  /**
   * Returns whether the shared state has requested closing.
   */
  [[nodiscard]] bool closing_requested() const noexcept;

  /**
   * Returns whether stop() has been called on this context.
   */
  [[nodiscard]] bool stop_requested() const noexcept;

  /**
   * Returns whether the context should leave the running state.
   */
  [[nodiscard]] bool should_finish() const noexcept;

  /**
   * Drains work and marks the context finished.
   */
  void finish() noexcept;

  /**
   * Waits for at least one CQE event on the native ring descriptor.
   */
  [[nodiscard]] int wait_for_cqe_event(
      __kernel_timespec* timeout = nullptr) noexcept;

  /**
   * Collects and dispatches ready CQE-backed tasks.
   */
  [[nodiscard]] bool collect_ready_cqes() noexcept;

  /**
   * Collects ready CQEs into an operation queue.
   */
  [[nodiscard]] unsigned collect_cqe_tasks(operation_queue& cqe_tasks) noexcept;

  /**
   * Dispatches collected CQE tasks locally or through the shared CPU queue.
   */
  void dispatch_cqe_tasks(operation_queue& cqe_tasks,
                          unsigned task_count) noexcept;

  /**
   * Enqueues the operation represented by one CQE data record.
   */
  [[nodiscard]] bool enqueue_cqe_task(const cqe_data& data,
                                      operation_queue& tasks) noexcept;

  /**
   * Initialises the ring with the supplied flags, retrying without
   * bnio-managed setup flags on EINVAL.
   *
   * @return 0 on success, or a negative errno.
   */
  int init_ring_params(unsigned entries, unsigned flags,
                       bnio::base::params& queue_params) noexcept;

  /**
   * Applies configuration from options to context member variables.
   */
  void apply_context_options(const io_uring_context_options& options) noexcept;

  /**
   * Verifies in debug builds that the context is running.
   */
  void assert_running() const noexcept;

  bnio::base::ring ring_;
  io_uring_context_options options_{};
  unsigned kernel_features_ = 0;
  std::atomic<context_state> state_{context_state::finished};
  bool queue_initialized_ = false;
  bool ring_disabled_ = false;

  std::atomic_bool run_active_{false};
  std::atomic_bool waiting_{false};
  bool eventfd_poll_pending_ = false;

  static thread_local io_uring_context* current_context_;
  io_uring_task_queue_state* global_state_ = nullptr;
  operation_queue local_tasks_;
  // Reserved for future ring-local I/O batching.
  operation_queue local_io_tasks_;
  io_uring_io_operation_base* inflight_io_head_ = nullptr;
  unsigned local_task_budget_ = 0;
};

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_CONTEXT_H_
