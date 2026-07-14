#pragma once
#ifndef BUPP_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_CONTEXT_H_
#define BUPP_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_CONTEXT_H_

#include <bupp/async_io/bsd/kqueue_context_base/operation_base.h>
#include <bupp/async_io/bsd/kqueue_context_base/options.h>
#include <bupp/async_io/bsd/kqueue_helper.h>
#include <bupp/async_io/descriptor_view.h>
#include <bupp/async_io/time.h>
#include <bupp/base/bsd/kqueue.h>
#include <bupp/export.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace bupp::async_io::bsd_native {

/** Single-threaded run loop backed by a BSD kqueue. */
class BUPP_EXPORT kqueue_context {
 public:
  using steady_clock = bupp::async_io::steady_clock;
  using clock = bupp::async_io::clock;
  using system_clock = bupp::async_io::system_clock;
  using duration = bupp::async_io::duration;
  using time_point = bupp::async_io::time_point;
  using system_time_point = bupp::async_io::system_time_point;

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

 private:
  struct operation_queue {
    void push(kqueue_operation_base& operation) noexcept;

    void push(kqueue_operation_base* operations) noexcept;

    [[nodiscard]] kqueue_operation_base* pop_all() noexcept;

    kqueue_operation_base* head = nullptr;
  };

  struct prepared_operation {
    kqueue_io_operation_base* operation = nullptr;
    std::array<bupp::base::event, 2> events{};
    std::size_t event_count = 0;
    kqueue_task task = kqueue_task::none;
    unsigned poll_mask = 0;
  };

  struct active_registration {
    kqueue_io_operation_base* operation = nullptr;
    std::uintptr_t descriptor = 0;
    std::int16_t filter = 0;
    kqueue_task task = kqueue_task::none;
    unsigned poll_mask = 0;
  };

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

  void push_cpu_task(kqueue_operation_base& operation) noexcept;
  void push_cpu_tasks(operation_queue& operations) noexcept;
  [[nodiscard]] bool move_cpu_tasks() noexcept;

  /** Consumes every local and shared I/O task after ready CPU work. */
  [[nodiscard]] bool consume_io_tasks() noexcept;

  [[nodiscard]] int prepare_io(kqueue_io_operation_base& operation,
                               prepared_operation& prepared) noexcept;

  void begin_wait() noexcept;
  void end_wait() noexcept;

  [[nodiscard]] int trigger_wakeup() noexcept;
  [[nodiscard]] static void* wakeup_user_data() noexcept;

  [[nodiscard]] run_phase handle_run_ready_tasks() noexcept;
  [[nodiscard]] run_phase handle_wait_for_work() noexcept;
  [[nodiscard]] run_phase handle_finish_drain() noexcept;
  [[nodiscard]] run_phase spin_for_work() noexcept;
  [[nodiscard]] run_phase wait_for_io_work() noexcept;
  [[nodiscard]] bool should_finish() const noexcept;
  void finish() noexcept;

  [[nodiscard]] bool collect_ready_events(bool wait) noexcept;
  [[nodiscard]] unsigned collect_event_tasks(operation_queue& event_tasks,
                                             bool wait) noexcept;
  void dispatch_event_tasks(operation_queue& event_tasks,
                            unsigned task_count) noexcept;
  [[nodiscard]] bool process_event(const bupp::base::event& event,
                                   operation_queue& tasks) noexcept;

  [[nodiscard]] int register_operation(
      const prepared_operation& operation) noexcept;
  void unregister_operation(kqueue_io_operation_base& operation) noexcept;
  [[nodiscard]] bool take_registration(
      const bupp::base::event& event,
      active_registration& registration) noexcept;
  [[nodiscard]] int perform_io(kqueue_io_operation_base& operation,
                               kqueue_task task, int descriptor) noexcept;
  [[nodiscard]] unsigned poll_result(
      unsigned poll_mask, const bupp::base::event& event) const noexcept;

  bupp::base::kqueue queue_;
  kqueue_context_options options_{};
  std::atomic<context_state> state_{context_state::finished};
  bool queue_initialized_ = false;

  std::unique_ptr<active_registration[]> active_registrations_;
  std::size_t active_registration_capacity_ = 0;

  std::unique_ptr<bupp::base::event[]> event_buffer_;
  std::atomic_bool run_active_{false};
  std::atomic_bool waiting_{false};

  static thread_local kqueue_context* current_context_;
  kqueue_task_queue_state* global_state_ = nullptr;
  operation_queue local_tasks_;
  kqueue_io_operation_base* local_io_tasks_ = nullptr;
  unsigned local_task_budget_ = 0;
};

}  // namespace bupp::async_io::bsd_native

#endif  // BUPP_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_CONTEXT_H_
