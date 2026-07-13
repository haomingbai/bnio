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
#include <mutex>

namespace bupp::async_io::bsd_native {

/** Run loop and submission context backed by a BSD kqueue. */
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

  /** Prepares an operation without registering it yet. */
  template <class Operation>
  int prepare(Operation& operation) noexcept;

  /** Registers all prepared operations. */
  int submit() noexcept;

  /** Prepares and registers one operation. */
  template <class Operation>
  int submit(Operation& operation) noexcept;

  /** Runs batched preparation while holding the submission gate once. */
  template <class Function>
  void submit_batch(Function&& function) noexcept;

  /** Prepares one operation while the submission gate is held. */
  template <class Operation>
  int prepare_locked(Operation& operation) noexcept;

  /** Registers prepared operations while the submission gate is held. */
  int submit_locked() noexcept;

  /** Wakes all run-loop waiters. */
  void notify_waiters() noexcept;

  /** Wakes one run-loop waiter. */
  void notify_one_waiter() noexcept;

  /** Posts an operation to the context run loop. */
  int post(kqueue_operation_base& operation) noexcept;

 private:
  using operation_queue = kqueue_operation_stack_state;

  struct prepared_operation {
    kqueue_operation_base* operation = nullptr;
    std::array<bupp::base::event, 2> events{};
    std::size_t event_count = 0;
    kqueue_task task = kqueue_task::none;
    unsigned poll_mask = 0;
  };

  struct active_registration {
    kqueue_operation_base* operation = nullptr;
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

  void push_posted_task(kqueue_operation_base& operation) noexcept;
  void push_posted_tasks(operation_queue& operations) noexcept;
  [[nodiscard]] bool move_posted_tasks(operation_queue& local_tasks) noexcept;

  [[nodiscard]] int trigger_wakeup() noexcept;
  [[nodiscard]] static void* wakeup_user_data() noexcept;

  [[nodiscard]] run_phase handle_run_ready_tasks(
      operation_queue& local_tasks, unsigned& local_task_budget) noexcept;
  [[nodiscard]] run_phase handle_wait_for_work(
      operation_queue& local_tasks, unsigned& local_task_budget) noexcept;
  [[nodiscard]] run_phase handle_finish_drain(
      operation_queue& local_tasks, unsigned& local_task_budget) noexcept;
  [[nodiscard]] run_phase spin_for_work(operation_queue& local_tasks,
                                        unsigned& local_task_budget) noexcept;
  [[nodiscard]] run_phase wait_for_io_work(
      operation_queue& local_tasks, unsigned& local_task_budget) noexcept;
  [[nodiscard]] bool should_finish() const noexcept;
  void finish(operation_queue& local_tasks,
              unsigned& local_task_budget) noexcept;

  [[nodiscard]] bool collect_ready_events(operation_queue& local_tasks,
                                          unsigned& local_task_budget,
                                          bool wait) noexcept;
  [[nodiscard]] unsigned collect_event_tasks(operation_queue& event_tasks,
                                             bool wait) noexcept;
  void dispatch_event_tasks(operation_queue& event_tasks, unsigned task_count,
                            operation_queue& local_tasks,
                            unsigned& local_task_budget) noexcept;
  [[nodiscard]] bool process_event(const bupp::base::event& event,
                                   operation_queue& tasks) noexcept;

  [[nodiscard]] int register_operation(
      const prepared_operation& operation) noexcept;
  void unregister_operation(kqueue_operation_base& operation) noexcept;
  [[nodiscard]] bool take_registration(
      const bupp::base::event& event,
      active_registration& registration) noexcept;
  [[nodiscard]] int perform_io(kqueue_operation_base& operation,
                               kqueue_task task, int descriptor) noexcept;
  [[nodiscard]] unsigned poll_result(
      unsigned poll_mask, const bupp::base::event& event) const noexcept;

  bupp::base::kqueue queue_;
  std::atomic<context_state> state_{context_state::finished};
  bool queue_initialized_ = false;

  std::unique_ptr<prepared_operation[]> prepared_operations_;
  std::size_t prepared_count_ = 0;
  std::size_t prepared_capacity_ = 0;
  std::mutex submission_mutex_;

  std::unique_ptr<active_registration[]> active_registrations_;
  std::size_t active_registration_capacity_ = 0;
  std::mutex registrations_mutex_;

  std::unique_ptr<bupp::base::event[]> event_buffer_;
  unsigned event_batch_window_ = kqueue_context_options{}.event_batch_window;
  unsigned wait_spin_count_ = kqueue_context_options{}.wait_spin_count;
  unsigned event_inline_completion_threshold_ =
      kqueue_context_options{}.event_inline_completion_threshold;
  unsigned local_queue_threshold_ =
      kqueue_context_options{}.local_queue_threshold;
  std::uintptr_t wakeup_ident_ = kqueue_context_options{}.wakeup_ident;

  operation_queue posted_tasks_;
  std::mutex posted_tasks_mutex_;
  std::atomic_bool run_active_{false};

  static thread_local kqueue_context* current_context_;
  operation_queue* local_tasks_ = nullptr;
};

}  // namespace bupp::async_io::bsd_native

#endif  // BUPP_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_CONTEXT_H_
