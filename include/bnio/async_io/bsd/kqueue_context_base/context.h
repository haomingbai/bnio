/**
 * @file context.h
 * @brief kqueue_context class declaration.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_CONTEXT_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_CONTEXT_H_

#include <bnio/async_io/bsd/kqueue_context_base/operation_base.h>
#include <bnio/async_io/bsd/kqueue_context_base/options.h>
#include <bnio/async_io/bsd/kqueue_helper.h>
#include <bnio/async_io/buffer_view.h>
#include <bnio/async_io/descriptor_view.h>
#include <bnio/async_io/dns.h>
#include <bnio/async_io/ip/endpoint.h>
#include <bnio/async_io/random_access_file.h>
#include <bnio/async_io/socket_view.h>
#include <bnio/async_io/time.h>
#include <bnio/base/bsd/kqueue.h>
#include <bnio/export.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace bnio::async_io::bsd_native {

/**
 * Passive readiness event loop backed by a BSD kqueue.
 *
 * Owns one base::kqueue. Single-owner and non-reentrant: concurrent run()
 * calls are undefined behavior, and run() must not be called again once
 * the loop has finished.
 */
class BNIO_EXPORT kqueue_context {
 public:
  /** Monotonic clock used by async I/O scheduling. */
  using steady_clock = bnio::async_io::steady_clock;

  /** Default async I/O scheduling clock. */
  using clock = bnio::async_io::clock;

  /** Wall-clock type for APIs that need system time. */
  using system_clock = bnio::async_io::system_clock;

  /** Canonical async I/O duration. */
  using duration = bnio::async_io::duration;

  /** Time point using the default async I/O clock. */
  using time_point = bnio::async_io::time_point;

  /** System-clock time point with async I/O duration precision. */
  using system_time_point = bnio::async_io::system_time_point;

  /** Creates a closed context. */
  kqueue_context() noexcept;

  /** Creates a context and attempts to open its kqueue. */
  explicit kqueue_context(const kqueue_context_options& options) noexcept;

  /** Stops and releases the context kqueue. */
  ~kqueue_context() noexcept;

  /** Disabled: the context owns a kqueue. */
  kqueue_context(const kqueue_context&) = delete;

  /** Disabled: the context owns a kqueue. */
  kqueue_context& operator=(const kqueue_context&) = delete;

  /** Disabled: owns a kqueue and single-owner run-loop state. */
  kqueue_context(kqueue_context&&) = delete;

  /** Disabled: owns a kqueue and single-owner run-loop state. */
  kqueue_context& operator=(kqueue_context&&) = delete;

  /** Initializes the native queue and wakeup event. */
  int queue_init(const kqueue_context_options& options = {}) noexcept;

  /**
   * Releases the native queue and pending storage. An unclean teardown
   * aborts inflight and queued I/O and runs every completion synchronously
   * on the calling thread; a finished context leaves sibling shared queues
   * untouched.
   */
  void queue_exit() noexcept;

  /** Returns whether the native queue is open. */
  [[nodiscard]] bool is_open() const noexcept;

  /** Creates a typed poll sender. */
  [[nodiscard]] auto async_poll(descriptor_view descriptor, unsigned poll_mask);

  /** ::read sender advancing the kernel file position. */
  [[nodiscard]] auto async_read(descriptor_view descriptor, buffer_view buffer);

  /** @c pread() sender at @p offset; does not advance the file position. */
  [[nodiscard]] auto async_read(random_access_file file, buffer_view buffer,
                                std::uint64_t offset);

  /** ::write sender advancing the kernel file position. */
  [[nodiscard]] auto async_write(descriptor_view descriptor, const void* data,
                                 std::size_t size);

  /** @c pwrite() sender at @p offset; does not advance the file position. */
  [[nodiscard]] auto async_write(random_access_file file, const void* data,
                                 std::size_t size, std::uint64_t offset);

  /** Creates one nonblocking stream receive sender. */
  [[nodiscard]] auto async_receive(stream_socket_view socket,
                                   buffer_view buffer, int flags = 0);

  /** Creates one nonblocking stream send sender. */
  [[nodiscard]] auto async_send(stream_socket_view socket, const void* data,
                                std::size_t size, int flags = 0);

  /** Creates one nonblocking connected-datagram receive sender. */
  [[nodiscard]] auto async_receive(datagram_socket_view socket,
                                   buffer_view buffer, int flags = 0);

  /** Creates one nonblocking connected-datagram send sender. */
  [[nodiscard]] auto async_send(datagram_socket_view socket, const void* data,
                                std::size_t size, int flags = 0);

  /** Creates one endpoint-aware datagram receive sender. */
  [[nodiscard]] auto async_receive_from(datagram_socket_view socket,
                                        buffer_view buffer,
                                        ip::endpoint& endpoint, int flags = 0);

  /** Creates one endpoint-aware datagram send sender. */
  [[nodiscard]] auto async_send_to(datagram_socket_view socket,
                                   const void* data, std::size_t size,
                                   const ip::endpoint& endpoint, int flags = 0);

  /** Creates one nonblocking accept sender. */
  [[nodiscard]] auto async_accept(stream_socket_view socket, int flags = 0);

  /** Creates one nonblocking connect sender. */
  [[nodiscard]] auto async_connect(stream_socket_view socket,
                                   const ip::endpoint& endpoint);

  /** Creates a DNS sender completed on the context run loop. */
  [[nodiscard]] auto async_resolve(bnio::async_io::dns_query query,
                                   bnio::async_io::dns_result_view result);

  /** Creates a DNS sender from host and service strings. */
  [[nodiscard]] auto async_resolve(std::string_view host,
                                   std::string_view service,
                                   bnio::async_io::dns_result_view result);

  /**
   * Runs posted work and readiness completions until stopped.
   *
   * Calling run() again after the loop has finished is unsupported.
   */
  void run() noexcept;

  /** Requests run-loop termination. */
  int stop() noexcept;

  /** Returns whether this context is running on the current thread. */
  [[nodiscard]] bool is_in_context() const noexcept;

  /**
   * Selects externally owned shared state for a worker group; null selects
   * the single-threaded local queues. The state must stay valid until this
   * context stops running.
   */
  void set_global_state(kqueue_task_queue_state* state) noexcept;

  /** Returns the bound shared task queue state, or null before run(). */
  [[nodiscard]] kqueue_task_queue_state* get_global_state() const noexcept {
    return global_state_;
  }

  /** Wakes one run-loop waiter. */
  void notify_one_waiter() noexcept;

  /** Returns whether this run-loop worker has published a sleeping state. */
  [[nodiscard]] bool is_waiting() const noexcept;

  /**
   * Returns the negative errno that aborted enter_run(), or 0 when run()
   * entered normally or never ran. Always 0 for kqueue; present for parity
   * with the io_uring backend.
   */
  [[nodiscard]] int enter_run_error() const noexcept { return 0; }

  /**
   * Posts an operation to the context run loop. Internal submission API:
   * always enqueues and never refuses, so the caller must keep the context
   * running until it completes. @see bnio::io_context::publish_cpu
   */
  int post(kqueue_operation_base& operation) noexcept;

  /**
   * Publishes I/O for passive preparation by the context run loop.
   * Internal API: always enqueues and never refuses, so the caller must
   * keep the context running until it completes.
   * @see bnio::io_context::publish_io
   */
  void publish_io(kqueue_io_operation_base& operation) noexcept;

  /**
   * Returns the worker-local task queue state, linked into the shared
   * suspend list while the worker sleeps. Its CPU queue is owned solely by
   * the worker thread.
   */
  [[nodiscard]] kqueue_local_task_queue_state* local_state() noexcept {
    return &local_state_;
  }

  /** Fetches a CPU-task batch from the local queue first, then the shared
   *  queue; stops at the first source that yields work. */
  [[nodiscard]] kqueue_operation_base* fetch_cpu_task() noexcept;

 private:
  struct operation_queue {
    void push(kqueue_operation_base& operation) noexcept;

    void push(kqueue_operation_base* operations) noexcept;

    [[nodiscard]] kqueue_operation_base* pop_all() noexcept;

    kqueue_operation_base* head = nullptr;
  };

  /** Unregisters this worker's local state from the shared list. */
  void unregister_local_state() noexcept;

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

  /** Applies configuration from options to context member variables. */
  void apply_context_options(const kqueue_context_options& options) noexcept;
  /** Verifies in debug builds that the context is running. */
  void assert_running() const noexcept;

  /** Initialises run-loop state. On failure the caller must exit early
   *  after restoring current_context_. */
  [[nodiscard]] bool enter_run() noexcept;

  /**
   * Registers a wake-channel fd as EVFILT_READ | EV_ADD | EV_CLEAR, so it
   * re-fires only after the channel is drained. The `udata` tag lets
   * process_event() filter the event out of operation dispatch; the
   * control result is ignored.
   */
  void register_wake_poll(int fd, void* udata) noexcept;

  /** Repeatedly drains timer expirations and local CPU tasks until the CPU
   *  queue is empty; used by finish() for abort-generated tasks. */
  void drain_local_cpu_tasks() noexcept;

  /** Publishes a local queue's operations to the shared CPU-task queue. */
  void push_cpu_tasks(operation_queue& operations) noexcept;
  /** Fetches and executes a batch of CPU tasks. Returns true if work ran. */
  [[nodiscard]] bool run_cpu_batch() noexcept;

  /**
   * Consumes staged I/O tasks, local queue first, then the shared queue.
   *
   * Once stop or close is requested, popped operations complete as stopped
   * instead of being registered: EV_ADD still succeeds on an open kqueue,
   * so arming them could starve the finish() break condition.
   */
  [[nodiscard]] bool consume_io_tasks() noexcept;

  /** Adds an I/O operation to the inflight doubly-linked list. */
  void add_inflight(kqueue_io_operation_base& operation) noexcept;

  /** Removes an I/O operation from the inflight doubly-linked list. */
  void remove_inflight(kqueue_io_operation_base& operation) noexcept;

  /** Aborts all inflight I/O operations during shutdown. */
  void abort_inflight_io() noexcept;

  /** Completes a linked list of unregistered I/O operations as stopped and
   *  pushes them to the local CPU queue. */
  void drain_io_list_complete_stopped(kqueue_io_operation_base* head) noexcept;

  /**
   * Prepares and registers one I/O operation with kqueue; @return true if
   * it was registered and is now inflight, false if it already completed
   * and the caller must push it to the CPU queue.
   */
  [[nodiscard]] bool prepare_and_register_operation(
      kqueue_io_operation_base& operation) noexcept;

  /** Moves due passive-timer completions into the local CPU queue. */
  [[nodiscard]] bool consume_timeout_operations() noexcept;

  /** Prepares one operation via kqueue_helper; returns its error or -EINVAL
   *  when no native action was selected. */
  [[nodiscard]] int prepare_io(kqueue_io_operation_base& operation) noexcept;

  /**
   * Publishes the sleeping state and links the worker into the shared
   * suspend list before a blocking wait.
   */
  void begin_wait() noexcept;
  /** Unlinks the worker from the suspend list and clears the sleeping
   *  state after a wait. */
  void end_wait() noexcept;

  /** Wakes a sleeping run loop through the shared wake channel (written
   *  under submit_lock), or the local EVFILT_USER trigger in standalone
   *  mode. */
  [[nodiscard]] int trigger_wakeup() noexcept;
  /** Returns the sentinel udata tagging shared wake-channel events. */
  [[nodiscard]] static void* wakeup_user_data() noexcept;
  /** Returns the sentinel udata tagging this context's own local wakeup
   *  events. */
  [[nodiscard]] static void* local_wakeup_user_data() noexcept;

  /** Kind of a kevent udata pointer, used to dispatch collected events. */
  enum class event_udata_kind {
    shared_wake,
    local_wake,
    operation,
  };

  /** Classifies a kevent udata value into a dispatch kind. */
  [[nodiscard]] static event_udata_kind classify_udata(void* udata) noexcept;

  /** Runs ready kevents, CPU tasks, and due timers. */
  [[nodiscard]] run_phase handle_run_ready_tasks() noexcept;
  /** Waits for work when no tasks are immediately ready. */
  [[nodiscard]] run_phase handle_wait_for_work() noexcept;
  /** Drains remaining work during shutdown. */
  [[nodiscard]] run_phase handle_finish_drain() noexcept;
  /** Spins briefly for readiness events or posted tasks before blocking. */
  [[nodiscard]] run_phase spin_for_work() noexcept;
  /** Waits for readiness events from the kqueue. */
  [[nodiscard]] run_phase wait_for_io_work() noexcept;

  /**
   * Fetches due timer operations, stores the earliest deadline, and fills
   * @p timeout with the time remaining; @p timeout_pointer is null when
   * unbounded. Returns true when work was found or finish was requested.
   */
  [[nodiscard]] bool compute_io_wait_timeout(
      async_io::time_point& deadline, timespec& timeout,
      const timespec*& timeout_pointer) noexcept;

  /** Returns whether the shared state has requested closing. */
  [[nodiscard]] bool closing_requested() const noexcept;
  /** Returns whether stop() has been called on this context. */
  [[nodiscard]] bool stop_requested() const noexcept;
  /** Returns whether the context should leave the running state. */
  [[nodiscard]] bool should_finish() const noexcept;
  /**
   * Drains ready events, CPU tasks, timers, and inflight I/O, then marks the
   * context finished; every published operation reaches a terminal receiver
   * call before the loop exits.
   */
  void finish() noexcept;

  /** Collects ready kevents and dispatches their tasks; @p wait blocks for
   *  the first event using @p timeout (null means unbounded). Returns true
   *  if work ran. */
  [[nodiscard]] bool collect_ready_events(
      bool wait, const timespec* timeout = nullptr) noexcept;
  /** Collects ready kevents into an operation queue and returns the task
   *  count. */
  [[nodiscard]] unsigned collect_event_tasks(operation_queue& event_tasks,
                                             bool wait,
                                             const timespec* timeout) noexcept;
  /** Dispatches collected event tasks locally or through the shared CPU
   *  queue. */
  void dispatch_event_tasks(operation_queue& event_tasks,
                            unsigned task_count) noexcept;
  /** Handles one collected kevent, resolving wake-channel drains and
   *  operation events. Returns true when an operation task was produced. */
  [[nodiscard]] bool process_event(const bnio::base::event& event,
                                   operation_queue& tasks) noexcept;
  /**
   * Drains the wake channel named by `udata`.
   * @return true if `udata` is a wake channel carrying no operation.
   */
  [[nodiscard]] bool drain_wake_channel(void* udata) noexcept;
  /** Finds the registration node of `operation` matching `filter`, or null. */
  [[nodiscard]] kqueue_registration_state* find_fired_node(
      kqueue_io_operation_base& operation, std::int16_t filter) noexcept;
  /** Resolves a fired event's result (poll mask, errno, write EOF, or the
   *  native I/O step with EAGAIN retry). Returns false when the operation
   *  was re-armed and must stay inflight. */
  [[nodiscard]] bool dispatch_event_result(
      kqueue_io_operation_base& operation, kqueue_registration_state& node,
      const bnio::base::event& event) noexcept;
  /** Performs the native I/O step; false means the operation was re-armed
   *  after EAGAIN and must stay inflight. */
  [[nodiscard]] bool perform_io_step(kqueue_io_operation_base& operation,
                                     kqueue_registration_state& node) noexcept;
  /** Resolves a fired write event: kevent errno, EOF, or the I/O step. */
  [[nodiscard]] bool dispatch_write_result(
      kqueue_io_operation_base& operation, kqueue_registration_state& node,
      const bnio::base::event& event) noexcept;
  /** Resolves a fired read (or fallback) event: kevent errno or the I/O
   *  step. */
  [[nodiscard]] bool dispatch_read_result(
      kqueue_io_operation_base& operation, kqueue_registration_state& node,
      const bnio::base::event& event) noexcept;

  /** Links every node of the operation into its (ident, filter) wait
   *  queue, rolling back on failure. */
  [[nodiscard]] int register_operation(
      kqueue_io_operation_base& operation) noexcept;
  /** Arms one node's kevent (EV_ADD | EV_RECEIPT) and marks it armed. */
  [[nodiscard]] int arm_registration(kqueue_registration_state& node) noexcept;
  /** Arms the first armable node at or after `candidate`, failing and
   *  detaching any node whose arming fails. `candidate` is the successor of
   *  a node that just left its wait queue. */
  void arm_queue_head(kqueue_registration_state* candidate) noexcept;
  /** Disarms and unlinks every node of the operation. */
  void unregister_operation(kqueue_io_operation_base& operation) noexcept;
  /** Translates a fired kevent into the subset of the requested poll mask
   *  it satisfies. */
  [[nodiscard]] unsigned poll_result(
      unsigned poll_mask, const bnio::base::event& event) const noexcept;

  /** Rearms a node after EAGAIN/EWOULDBLOCK; @return true on success (do not
   *  complete), false on failure (operation.result has been set). */
  [[nodiscard]] bool try_rearm_operation(
      kqueue_io_operation_base& operation,
      kqueue_registration_state& node) noexcept;

  /** @brief Wait-queue helpers backed by the inflight list (no allocation). */
  [[nodiscard]] kqueue_registration_state* find_queue_tail(
      std::uintptr_t ident, std::int16_t filter) const noexcept;
  [[nodiscard]] int append_node(kqueue_registration_state& node) noexcept;
  kqueue_registration_state* unlink_node(
      kqueue_registration_state& node) noexcept;
  /** Tears down an operation whose arming failed: unlinks its nodes without
   *  rearming, removes it from inflight, and completes it with `result`. */
  void fail_operation(kqueue_io_operation_base& operation, int result) noexcept;

  /** Run-loop lifecycle and run flags. */
  struct run_state {
    /** Overall lifecycle state (running / finishing / finished). */
    std::atomic<context_state> state{context_state::finished};
    /** Whether a run loop is active on this context. */
    std::atomic_bool run_active{false};
    /** Whether this worker has published a sleeping state. */
    std::atomic_bool waiting{false};
    /** Whether queue_init() has completed. */
    bool queue_initialized = false;
  };

  /** Scheduling cursors and sequence counters. */
  struct scheduling_state {
    /** Remaining local inline-completion budget for this round. */
    unsigned local_task_budget = 0;
    /** Monotonic registration sequence; mirrors wait-queue insertion order. */
    std::uint64_t next_registration_sequence = 0;
  };

  bnio::base::kqueue queue_;
  kqueue_context_options options_{};
  run_state run_state_;
  scheduling_state scheduling_state_;

  std::unique_ptr<bnio::base::event[]> event_buffer_;

  static thread_local kqueue_context* current_context_;
  kqueue_task_queue_state* global_state_ = nullptr;
  kqueue_local_task_queue_state local_state_;
  kqueue_io_operation_base* inflight_io_head_ = nullptr;
};

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_CONTEXT_H_
