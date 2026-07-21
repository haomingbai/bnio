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

 private:
  struct operation_queue {
    void push(kqueue_operation_base& operation) noexcept;

    void push(kqueue_operation_base* operations) noexcept;

    [[nodiscard]] kqueue_operation_base* pop_all() noexcept;

    kqueue_operation_base* head = nullptr;
  };

  struct local_task_queue_state {
    void push_cpu(kqueue_operation_base& operation) noexcept;
    void push_cpu(kqueue_operation_base* operations) noexcept;
    void push_io(kqueue_io_operation_base& operation) noexcept;
    void clear() noexcept;

    operation_queue cpu;
    kqueue_io_operation_base* io = nullptr;
  };

  struct prepared_operation {
    kqueue_io_operation_base* operation = nullptr;
    std::array<bnio::base::event, 2> events{};
    std::size_t event_count = 0;
    kqueue_task task = kqueue_task::none;
    unsigned poll_mask = 0;
  };

  struct active_registration {
    kqueue_io_operation_base* operation = nullptr;
    bnio::base::event event{};
    kqueue_task task = kqueue_task::none;
    unsigned poll_mask = 0;
    bool armed = false;
    std::uint64_t sequence = 0;
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

  void push_cpu_tasks(operation_queue& operations) noexcept;
  [[nodiscard]] bool consume_global_state() noexcept;
  [[nodiscard]] bool consume_local_state() noexcept;

  /** Consumes staged local I/O tasks after ready CPU work. */
  [[nodiscard]] bool consume_io_tasks() noexcept;

  /** Moves due passive-timer completions into the local CPU queue. */
  [[nodiscard]] bool consume_timeout_operations() noexcept;

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

  [[nodiscard]] bool collect_ready_events(
      bool wait, const timespec* timeout = nullptr) noexcept;
  [[nodiscard]] unsigned collect_event_tasks(operation_queue& event_tasks,
                                             bool wait,
                                             const timespec* timeout) noexcept;
  void dispatch_event_tasks(operation_queue& event_tasks,
                            unsigned task_count) noexcept;
  [[nodiscard]] bool process_event(const bnio::base::event& event,
                                   operation_queue& tasks) noexcept;

  [[nodiscard]] int register_operation(
      const prepared_operation& operation) noexcept;
  [[nodiscard]] int arm_registration(
      active_registration& registration) noexcept;
  void arm_next_registration(std::uintptr_t descriptor,
                             std::int16_t filter) noexcept;
  void unregister_operation(kqueue_io_operation_base& operation) noexcept;
  [[nodiscard]] bool take_registration(
      const bnio::base::event& event,
      active_registration& registration) noexcept;
  [[nodiscard]] unsigned poll_result(
      unsigned poll_mask, const bnio::base::event& event) const noexcept;

  /** @brief Finds a free registration slot, or nullptr if all are occupied. */
  [[nodiscard]] active_registration* find_free_registration_slot() noexcept;

  /** @brief Checks whether an armed registration already exists for
   * ident+filter. */
  [[nodiscard]] bool is_event_already_armed(std::uintptr_t ident,
                                            std::int16_t filter) const noexcept;

  /** @brief Attempts to rearm a registration after EAGAIN/EWOULDBLOCK.
   *
   * @return true if rearm succeeded (caller should not complete the operation),
   *         false if rearm failed and operation.result has been set.
   */
  [[nodiscard]] bool try_rearm_operation(
      kqueue_io_operation_base& operation,
      const active_registration& registration) noexcept;

  bnio::base::kqueue queue_;
  kqueue_context_options options_{};
  std::atomic<context_state> state_{context_state::finished};
  bool queue_initialized_ = false;

  std::unique_ptr<active_registration[]> active_registrations_;
  std::size_t active_registration_capacity_ = 0;
  std::uint64_t next_registration_sequence_ = 0;

  std::unique_ptr<bnio::base::event[]> event_buffer_;
  std::atomic_bool run_active_{false};
  std::atomic_bool waiting_{false};

  static thread_local kqueue_context* current_context_;
  kqueue_task_queue_state* global_state_ = nullptr;
  local_task_queue_state local_state_;
  kqueue_io_operation_base* incoming_io_tasks_ = nullptr;
  unsigned local_task_budget_ = 0;
};

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_CONTEXT_H_
