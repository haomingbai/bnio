#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_CONTEXT_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_CONTEXT_H_

#include <bupp/async_io/descriptor_view.h>
#include <bupp/async_io/dns.h>
#include <bupp/async_io/linux/io_uring_context_base/operation_base.h>
#include <bupp/async_io/linux/io_uring_context_base/options.h>
#include <bupp/async_io/time.h>
#include <bupp/base/linux/ring.h>
#include <bupp/base/linux/submission_queue_entry.h>
#include <bupp/export.h>

#include <atomic>
#include <cstdint>
#include <emmintrin.h>
#include <string_view>

namespace bupp::async_io::linux_native {

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
  [[nodiscard]] auto async_poll(bupp::async_io::descriptor_view descriptor,
                                unsigned poll_mask);

  /**
   * Creates a sender that resolves a DNS query into caller-provided result
   * storage on the context run loop.
   */
  [[nodiscard]] auto async_resolve(bupp::async_io::dns_query query,
                                   bupp::async_io::dns_result_view result);

  /**
   * Creates a sender that resolves a host and service into caller-provided
   * result storage on the context run loop.
   */
  [[nodiscard]] auto async_resolve(std::string_view host,
                                   std::string_view service,
                                   bupp::async_io::dns_result_view result);

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
   * Prepares an operation SQE without submitting the ring.
   */
  template <class Operation>
  int prepare(Operation& operation) noexcept;

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
  int submit(Operation& operation) noexcept;

  /**
   * Runs a batch of submission work while holding the uring gate once.
   *
   * The callback receives two callables: prepare(operation) reserves and fills
   * one SQE, and submit() submits all SQEs prepared so far.
   */
  template <class Function>
  void submit_batch(Function&& fn) noexcept;

  /**
   * RAII guard for exclusive access to the io_uring SQ/CQ rings.
   */
  class uring_lock {
   public:
    uring_lock() noexcept = default;

    uring_lock(const uring_lock&) = delete;
    uring_lock& operator=(const uring_lock&) = delete;

    uring_lock(uring_lock&& other) noexcept : gate_(other.gate_) {
      other.gate_ = nullptr;
    }

    uring_lock& operator=(uring_lock&& other) noexcept {
      if (this != &other) {
        reset();
        gate_ = other.gate_;
        other.gate_ = nullptr;
      }
      return *this;
    }

    ~uring_lock() noexcept { reset(); }

    [[nodiscard]] explicit operator bool() const noexcept {
      return gate_ != nullptr;
    }

    void reset() noexcept {
      if (gate_ != nullptr) {
        gate_->store(1U, std::memory_order_release);
        gate_ = nullptr;
      }
    }

   private:
    friend class io_uring_context;

    explicit uring_lock(std::atomic<unsigned>& gate) noexcept : gate_(&gate) {}

    std::atomic<unsigned>* gate_ = nullptr;
  };

  /**
   * Issues a short processor pause while spinning.
   */
  static void pause_uring_spin() noexcept { _mm_pause(); }

  /**
   * Spins until exclusive io_uring access is acquired.
   */
  [[nodiscard]] uring_lock lock_uring() const noexcept {
    for (;;) {
      if (uring_gate_.load(std::memory_order_acquire) != 0U &&
          uring_gate_.exchange(0U, std::memory_order_acq_rel) != 0U) {
        return uring_lock(uring_gate_);
      }
      while (uring_gate_.load(std::memory_order_relaxed) == 0U) {
        pause_uring_spin();
      }
    }
  }

  /**
   * Attempts to acquire exclusive io_uring access.
   */
  [[nodiscard]] uring_lock try_lock_uring() const noexcept {
    if (uring_gate_.load(std::memory_order_acquire) == 0U ||
        uring_gate_.exchange(0U, std::memory_order_acq_rel) == 0U) {
      return uring_lock();
    }
    return uring_lock(uring_gate_);
  }

  /**
   * Prepares one operation into the submission queue while the caller holds
   * the io_uring gate.
   */
  template <class Operation>
  int prepare_locked(Operation& operation) noexcept;

  /**
   * Submits prepared queue entries while the caller holds the io_uring gate.
   */
  int submit_locked() noexcept;

  /**
   * Wakes the run loop through the context eventfd.
   */
  void notify_waiters() noexcept;

  /**
   * Wakes the run loop through the context eventfd.
   */
  void notify_one_waiter() noexcept;

  /**
   * Posts an operation for execution by the context run loop.
   */
  int post(io_uring_operation_base& operation) noexcept;

 private:
  /**
   * Intrusive stack of operations used by run-loop phases.
   */
  struct operation_queue;

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
   * Signals the eventfd used by cross-thread producers.
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
   * Submits the internal eventfd poll request while the uring gate is held.
   */
  [[nodiscard]] int submit_eventfd_poll_locked() noexcept;

  /**
   * Returns the sentinel user data used for eventfd poll SQEs.
   */
  [[nodiscard]] static void* eventfd_user_data() noexcept;

  /**
   * Runs ready local and global tasks.
   */
  [[nodiscard]] run_phase handle_run_ready_tasks(
      operation_queue& local_tasks, unsigned& local_task_budget) noexcept;

  /**
   * Waits for work when no tasks are immediately ready.
   */
  [[nodiscard]] run_phase handle_wait_for_work(
      operation_queue& local_tasks, unsigned& local_task_budget) noexcept;

  /**
   * Drains remaining work during shutdown.
   */
  [[nodiscard]] run_phase handle_finish_drain(
      operation_queue& local_tasks, unsigned& local_task_budget) noexcept;

  /**
   * Polls briefly for CQEs or posted tasks before blocking.
   */
  [[nodiscard]] run_phase spin_for_work(operation_queue& local_tasks,
                                        unsigned& local_task_budget) noexcept;

  /**
   * Waits for io_uring completion events.
   */
  [[nodiscard]] run_phase wait_for_io_work(
      operation_queue& local_tasks, unsigned& local_task_budget) noexcept;

  /**
   * Returns whether the context should leave the running state.
   */
  [[nodiscard]] bool should_finish() const noexcept;

  /**
   * Drains work and marks the context finished.
   */
  void finish(operation_queue& local_tasks,
              unsigned& local_task_budget) noexcept;

  /**
   * Waits for at least one CQE event on the native ring descriptor.
   */
  [[nodiscard]] int wait_for_cqe_event() noexcept;

  /**
   * Collects and dispatches ready CQE-backed tasks.
   */
  [[nodiscard]] bool collect_ready_cqes(operation_queue& local_tasks,
                                        unsigned& local_task_budget,
                                        bool wait_for_gate = false) noexcept;

  /**
   * Collects ready CQEs into an operation queue.
   */
  [[nodiscard]] unsigned collect_cqe_tasks(operation_queue& cqe_tasks,
                                           bool wait_for_gate) noexcept;

  /**
   * Dispatches collected CQE tasks locally or through the global queue.
   */
  void dispatch_cqe_tasks(operation_queue& cqe_tasks, unsigned task_count,
                          operation_queue& local_tasks,
                          unsigned& local_task_budget) noexcept;

  /**
   * Enqueues the operation represented by one CQE data record.
   */
  [[nodiscard]] bool enqueue_cqe_task(const cqe_data& data,
                                      operation_queue& tasks) noexcept;

  /**
   * Initialises the ring with the supplied flags, retrying without
   * bupp-managed setup flags on EINVAL.
   *
   * @return 0 on success, or a negative errno.
   */
  int init_ring_params(unsigned entries, unsigned flags,
                       bupp::base::params& queue_params) noexcept;

  /**
   * Applies configuration from options to context member variables.
   */
  void apply_context_options(const io_uring_context_options& options) noexcept;

  /**
   * Verifies in debug builds that the context is running.
   */
  void assert_running() const noexcept;

  bupp::base::ring ring_;
  mutable std::atomic<unsigned> uring_gate_{1U};
  unsigned kernel_features_ = 0;
  std::atomic<context_state> state_{context_state::finished};
  bool queue_initialized_ = false;

  std::atomic<io_uring_operation_base*> global_tasks_{nullptr};
  std::atomic_bool run_active_{false};
  int event_fd_ = -1;
  bool owns_event_fd_ = false;
  bool eventfd_poll_pending_ = false;

  unsigned wait_spin_count_ = io_uring_context_options{}.wait_spin_count;
  unsigned cqe_batch_window_ = io_uring_context_options{}.cqe_batch_window;
  unsigned cqe_inline_completion_threshold_ =
      io_uring_context_options{}.cqe_inline_completion_threshold;
  unsigned local_queue_threshold_ =
      io_uring_context_options{}.local_queue_threshold;

  static thread_local io_uring_context* current_context_;
  operation_queue* local_tasks_ = nullptr;
};

}  // namespace bupp::async_io::linux_native

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_CONTEXT_H_
