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
#include <bnio/async_io/socket_view.h>
#include <bnio/async_io/time.h>
#include <bnio/base/bsd/kqueue.h>
#include <bnio/export.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace bnio::async_io::bsd_native {

/** Single-threaded run loop backed by a BSD kqueue. */
class BNIO_EXPORT kqueue_context {
 public:
  using steady_clock = bnio::async_io::steady_clock;
  using clock = bnio::async_io::clock;
  using system_clock = bnio::async_io::system_clock;
  using duration = bnio::async_io::duration;
  using time_point = bnio::async_io::time_point;
  using system_time_point = bnio::async_io::system_time_point;

  /** Creates a closed context. */
  kqueue_context() noexcept;

  /** Creates a context and attempts to open its kqueue. */
  explicit kqueue_context(const kqueue_context_options& options) noexcept;

  /** Stops and releases the context kqueue. */
  ~kqueue_context() noexcept;

  kqueue_context(const kqueue_context&) = delete;
  kqueue_context& operator=(const kqueue_context&) = delete;
  kqueue_context(kqueue_context&&) = delete;
  kqueue_context& operator=(kqueue_context&&) = delete;

  /** Initializes the native queue and wakeup event. */
  int queue_init(const kqueue_context_options& options = {}) noexcept;

  /** Releases the native queue and pending internal storage. */
  void queue_exit() noexcept;

  /** Returns whether the native queue is open. */
  [[nodiscard]] bool is_open() const noexcept;

  /** Creates a typed poll sender. */
  [[nodiscard]] auto async_poll(descriptor_view descriptor, unsigned poll_mask);

  /** Creates a regular-file read sender whose start performs pread. */
  [[nodiscard]] auto async_read(descriptor_view descriptor, buffer_view buffer,
                                std::uint64_t offset = 0);

  /** Creates a regular-file write sender whose start performs pwrite. */
  [[nodiscard]] auto async_write(descriptor_view descriptor, const void* data,
                                 std::size_t size, std::uint64_t offset = 0);

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

  /** Runs posted work and readiness completions until stopped. */
  void run() noexcept;

  /** Requests run-loop termination. */
  int stop() noexcept;

  /** Returns whether this context is running on the current thread. */
  [[nodiscard]] bool is_in_context() const noexcept;

  /**
   * Selects externally owned shared state for a worker group.
   *
   * A null pointer selects this context's single-threaded local queues. The
   * state must remain valid until this context stops running.
   */
  void set_global_state(kqueue_task_queue_state* state) noexcept;

  /** Wakes one run-loop waiter. */
  void notify_one_waiter() noexcept;

  /** Returns whether this run-loop worker has published a sleeping state. */
  [[nodiscard]] bool is_waiting() const noexcept;

  /** Posts an operation to the context run loop. */
  int post(kqueue_operation_base& operation) noexcept;

  /** Publishes I/O for passive preparation by the context run loop. */
  void publish_io(kqueue_io_operation_base& operation) noexcept;

  /**
   * Returns the worker-local task queue state.
   *
   * The state is linked into the shared task queue state's local_states
   * list while the worker is running. Remote threads must hold the run
   * list's lock (workers.run.lock) while touching it (see steal_cpu_tasks).
   */
  [[nodiscard]] kqueue_local_task_queue_state* local_state() noexcept {
    return &local_state_;
  }

  /**
   * Fetches a batch of CPU tasks, trying the local queue first, then the
   * shared queue, then remote stealing. Stops at the first source that
   * yields tasks so work never accumulates on this thread's stack.
   */
  [[nodiscard]] kqueue_operation_base* fetch_cpu_task() noexcept;

 private:
  struct operation_queue {
    void push(kqueue_operation_base& operation) noexcept;

    void push(kqueue_operation_base* operations) noexcept;

    [[nodiscard]] kqueue_operation_base* pop_all() noexcept;

    kqueue_operation_base* head = nullptr;
  };

  /**
   * Attempts to steal a batch of CPU tasks from another worker's local
   * queue. Holds the run list's lock for the whole traversal; the lock also
   * keeps the visited worker from being destroyed mid-steal.
   */
  [[nodiscard]] kqueue_operation_base* steal_cpu_tasks() noexcept;

  /** Registers this worker's local state in the shared list (head insert). */
  void register_local_state() noexcept;

  /** Unregisters this worker's local state from the shared list. */
  void unregister_local_state() noexcept;

  enum class context_state {
    running,
    finishing,
    finished,
  };

  enum class run_phase {
    run_ready_tasks,
    wait_for_work,
    finish_drain,
    finished,
  };

  void apply_context_options(const kqueue_context_options& options) noexcept;
  void assert_running() const noexcept;

  /**
   * Initialises run-loop state and returns whether the worker may proceed.
   *
   * On failure the caller must exit early after restoring current_context_.
   */
  [[nodiscard]] bool enter_run() noexcept;

  /**
   * Repeatedly drains timer expirations and local CPU tasks until the
   * CPU queue is empty. Used by finish() to drain abort-generated tasks.
   */
  void drain_local_cpu_tasks() noexcept;

  void push_cpu_tasks(operation_queue& operations) noexcept;
  /** Fetches and executes a batch of CPU tasks. Returns true if work ran. */
  [[nodiscard]] bool run_cpu_batch() noexcept;

  /** Consumes staged local I/O tasks after ready CPU work. */
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
   * Prepares and registers one I/O operation with kqueue.
   *
   * @return true if the operation was registered and added to the inflight
   *         list; false if it completed immediately (preparation or
   *         registration failure, or a nop task), in which case the caller
   *         must push the operation to the CPU queue.
   */
  [[nodiscard]] bool prepare_and_register_operation(
      kqueue_io_operation_base& operation) noexcept;

  /** Moves due passive-timer completions into the local CPU queue. */
  [[nodiscard]] bool consume_timeout_operations() noexcept;

  [[nodiscard]] int prepare_io(kqueue_io_operation_base& operation) noexcept;

  void begin_wait() noexcept;
  void end_wait() noexcept;

  [[nodiscard]] int trigger_wakeup() noexcept;
  [[nodiscard]] static void* wakeup_user_data() noexcept;
  [[nodiscard]] static void* local_wakeup_user_data() noexcept;

  /** Classifies a kevent udata value into a dispatch kind. */
  enum class event_udata_kind {
    shared_wake,
    local_wake,
    operation,
  };

  [[nodiscard]] static event_udata_kind classify_udata(void* udata) noexcept;

  [[nodiscard]] run_phase handle_run_ready_tasks() noexcept;
  [[nodiscard]] run_phase handle_wait_for_work() noexcept;
  [[nodiscard]] run_phase handle_finish_drain() noexcept;
  [[nodiscard]] run_phase spin_for_work() noexcept;
  [[nodiscard]] run_phase wait_for_io_work() noexcept;

  /**
   * Fetches due timer operations from the shared heap and computes the wait
   * timeout for the native poller.
   *
   * @param[out] deadline        Timer deadline fetched from the shared heap.
   * @param[out] timeout         Filled with the remaining time until deadline.
   * @param[out] timeout_pointer Points at @p timeout when a deadline was
   *                             fetched; null for an unbounded wait.
   * @return true if work was found (timer operations pushed, a CPU batch ran,
   *         I/O was consumed, or finish was requested) and the caller should
   *         leave the wait.
   */
  [[nodiscard]] bool compute_io_wait_timeout(
      async_io::time_point& deadline, timespec& timeout,
      const timespec*& timeout_pointer) noexcept;

  [[nodiscard]] bool closing_requested() const noexcept;
  [[nodiscard]] bool stop_requested() const noexcept;
  [[nodiscard]] bool should_finish() const noexcept;
  void finish() noexcept;

  [[nodiscard]] bool collect_ready_events(
      bool wait, const timespec* timeout = nullptr) noexcept;
  [[nodiscard]] unsigned collect_event_tasks(operation_queue& event_tasks,
                                             bool wait,
                                             const timespec* timeout) noexcept;
  void dispatch_event_tasks(operation_queue& event_tasks,
                            unsigned task_count) noexcept;
  [[nodiscard]] bool process_event(const bnio::base::event& event,
                                   operation_queue& tasks) noexcept;
  /**
   * Resolves the result of a fired event on `operation`/`node` (poll mask,
   * kevent errno, write EOF, or the native I/O step with EAGAIN retry).
   * @return true if the operation should be completed; false if it was
   *         re-armed after EAGAIN and must stay inflight.
   */
  [[nodiscard]] bool dispatch_event_result(
      kqueue_io_operation_base& operation, kqueue_registration_state& node,
      const bnio::base::event& event) noexcept;

  [[nodiscard]] int register_operation(
      kqueue_io_operation_base& operation) noexcept;
  [[nodiscard]] int arm_registration(kqueue_registration_state& node) noexcept;
  /**
   * Arms the first armable node starting at `candidate`, looping past any
   * node whose arming fails (each such node's operation is failed and
   * detached). `candidate` is the successor of a node that just left its
   * wait queue.
   */
  void arm_queue_head(kqueue_registration_state* candidate) noexcept;
  void unregister_operation(kqueue_io_operation_base& operation) noexcept;
  [[nodiscard]] unsigned poll_result(
      unsigned poll_mask, const bnio::base::event& event) const noexcept;

  /** Attempts to rearm a node after EAGAIN/EWOULDBLOCK.
   *
   * @return true if rearm succeeded (caller should not complete the operation),
   *         false if rearm failed and operation.result has been set.
   */
  [[nodiscard]] bool try_rearm_operation(
      kqueue_io_operation_base& operation,
      kqueue_registration_state& node) noexcept;

  /** @brief Wait-queue helpers backed by the inflight list (no allocation).
   *
   * The wait queues live entirely inside the registration nodes embedded in
   * inflight operations. Finding a queue tail is a linear scan of the
   * inflight list; unlinking a node is O(1) via the doubly-linked wait
   * pointers.
   */
  [[nodiscard]] kqueue_registration_state* find_queue_tail(
      std::uintptr_t ident, std::int16_t filter) const noexcept;
  [[nodiscard]] int append_node(kqueue_registration_state& node) noexcept;
  kqueue_registration_state* unlink_node(
      kqueue_registration_state& node) noexcept;
  /** Tears down an operation whose arming failed: removes all its nodes
   *  (no re-arm), removes it from inflight, and completes it with `result`. */
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
    /** Steal start point for the next round; points at a node in the shared
     *  local_states list. Head insertion never invalidates it. */
    kqueue_local_task_queue_state* steal_cursor = nullptr;
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
  /** Standalone-mode IO queue (no global state); unused in multi-worker. */
  kqueue_io_operation_base* local_io_head_ = nullptr;
  kqueue_io_operation_base* inflight_io_head_ = nullptr;
};

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_CONTEXT_H_
