#pragma once
#ifndef BUPP_LINUX_IO_CONTEXT_H_
#define BUPP_LINUX_IO_CONTEXT_H_

#include <bupp/async_io/linux/io_uring_context.h>
#include <bupp/async_io/linux/io_uring_operations/poll.h>
#include <bupp/async_io/linux/socket_address.h>
#include <bupp/async_io/socket_view.h>
#include <bupp/async_io/time.h>
#include <bupp/buffer.h>
#include <bupp/export.h>
#include <bupp/io_context_cpo.h>
#include <bupp/ip.h>
#include <bupp/linux/detail/io_context_options.h>
#include <bupp/linux/detail/io_context_state.h>
#include <bupp/linux/detail/io_context_timer_types.h>
#include <bupp/linux/detail/steady_timer.h>
#include <linux/fs.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <bexec/completion_signatures.hpp>
#include <bexec/detail/manual_lifetime.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <bexec/repeat_until.hpp>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bupp {

enum class ssl_handshake_type;

namespace base {
class submission_queue_entry;
}  // namespace base

/**
 * High-level asynchronous I/O context for Linux.
 *
 * io_context adapts the non-owning async_io views into sender-returning
 * operations. Higher-level stream owners build on top of these view-level
 * operations instead of being known by io_context.
 */
class BUPP_EXPORT io_context {
 public:
  /**
   * Base class for operations scheduled by io_context.
   */
  class operation_base
      : public async_io::linux_native::io_uring_operation_base {
   public:
    /**
     * Creates an unqueued operation base.
     */
    operation_base() noexcept = default;

    /**
     * Copy construction is disabled because operations are queued
     * intrusively.
     */
    operation_base(const operation_base&) = delete;

    /**
     * Copy assignment is disabled because operations are queued intrusively.
     */
    operation_base& operator=(const operation_base&) = delete;

    /**
     * Move construction is disabled because operations are queued intrusively.
     */
    operation_base(operation_base&&) = delete;

    /**
     * Move assignment is disabled because operations are queued intrusively.
     */
    operation_base& operator=(operation_base&&) = delete;

    /**
     * Destroys the operation base.
     */
    ~operation_base() override = default;

    /**
     * Intrusive next pointer used by the shared I/O queue.
     */
    operation_base* pending_next = nullptr;

    /**
     * Fills one native submission queue entry for this operation.
     */
    virtual void prepare(base::submission_queue_entry& sqe) noexcept = 0;

    /**
     * Completes the operation when submission preparation fails.
     */
    virtual void complete_submit_error(int result) noexcept = 0;
  };

  /**
   * Monotonic clock used by this context.
   */
  using steady_clock = async_io::steady_clock;

  /**
   * Default clock used by this context.
   */
  using clock = async_io::clock;

  /**
   * Default duration type used by this context.
   */
  using duration = async_io::duration;

  /**
   * Default time point type used by this context.
   */
  using time_point = async_io::time_point;

  /**
   * Scheduling policy used by io_context scheduler handles.
   */
  enum class schedule_kind {
    /**
     * Complete schedule() inline when start() runs on the context thread.
     */
    dispatch,

    /**
     * Always post schedule() completion through the context run loop.
     */
    post,
  };

  /**
   * Sender returned by io_context schedulers' schedule() member.
   */
  template <schedule_kind Kind>
  class schedule_sender {
   public:
    /**
     * Completion signatures produced by the scheduler sender.
     */
    using completion_signatures =
        bexec::completion_signatures<bexec::set_value_t(),
                                     bexec::set_stopped_t()>;

    /**
     * Creates a schedule sender bound to context.
     */
    explicit schedule_sender(io_context& context) noexcept
        : context_(&context) {}

    /**
     * Operation state for a scheduler schedule() sender.
     */
    template <class Receiver>
    class operation : public async_io::linux_native::io_uring_operation_base {
     public:
      /**
       * Creates an operation bound to the context and receiver.
       */
      operation(io_context& context, Receiver receiver)
          : context_(&context), receiver_(std::move(receiver)) {}

      /**
       * Copy construction is disabled because operations are queued
       * intrusively.
       */
      operation(const operation&) = delete;

      /**
       * Copy assignment is disabled because operations are queued intrusively.
       */
      operation& operator=(const operation&) = delete;

      /**
       * Move construction is disabled because operations are queued
       * intrusively.
       */
      operation(operation&&) = delete;

      /**
       * Move assignment is disabled because operations are queued intrusively.
       */
      operation& operator=(operation&&) = delete;

      /**
       * Starts the schedule operation.
       */
      void start() noexcept {
        if constexpr (Kind == schedule_kind::dispatch) {
          if (context_->is_in_context()) {
            complete();
            return;
          }
        }

        context_->post(*this);
      }

      /**
       * Delivers the schedule completion.
       */
      void execute() noexcept override { complete(); }

     private:
      void complete() noexcept {
        auto env = bexec::get_env(receiver_);
        auto token = bexec::query(env, bexec::get_stop_token);
        if (token.stop_requested()) {
          bexec::set_stopped(std::move(receiver_));
        } else {
          bexec::set_value(std::move(receiver_));
        }
      }

      io_context* context_;
      Receiver receiver_;
    };

    template <class Receiver>
    /**
     * Connects the schedule sender to a receiver.
     */
    [[nodiscard]] auto connect(Receiver receiver) const {
      return operation<std::remove_cvref_t<Receiver>>(*context_,
                                                      std::move(receiver));
    }

   private:
    io_context* context_;
  };

  /**
   * Copyable scheduler handle produced by io_context.
   */
  template <schedule_kind Kind>
  class basic_scheduler {
   public:
    /**
     * Concrete sender type returned by schedule().
     */
    using schedule_sender_type = schedule_sender<Kind>;

    /**
     * Copies a scheduler handle.
     */
    basic_scheduler(const basic_scheduler&) noexcept = default;

    /**
     * Assigns a scheduler handle.
     */
    basic_scheduler& operator=(const basic_scheduler&) noexcept = default;

    /**
     * Returns a sender that completes according to this scheduler's policy.
     */
    [[nodiscard]] schedule_sender_type schedule() const noexcept {
      return schedule_sender_type(*context_);
    }

    /**
     * Returns the context that owns this scheduler.
     */
    [[nodiscard]] io_context& context() const noexcept { return *context_; }

    /**
     * Returns the native io_uring context used by this scheduler.
     */
    [[nodiscard]] async_io::linux_native::io_uring_context& native_context()
        const noexcept {
      return context_->native_context();
    }

    /**
     * Publishes one operation for passive io_uring submission by a worker.
     */
    void enqueue_io(operation_base& operation) const noexcept {
      context_->enqueue_io(operation);
    }

    /**
     * Posts one operation onto the owning context run loop.
     */
    void post(async_io::linux_native::io_uring_operation_base& operation)
        const noexcept {
      context_->post(operation);
    }

    /**
     * Creates a sender for one socket read operation.
     */
    [[nodiscard]] auto async_read(async_io::stream_socket_view socket,
                                  mutable_buffer buffer, int flags = 0) const;

    /**
     * Creates a sender for one socket read operation.
     */
    [[nodiscard]] auto async_read_some(async_io::stream_socket_view socket,
                                       mutable_buffer buffer,
                                       int flags = 0) const;

    /**
     * Creates a sender that writes the whole buffer to a socket.
     */
    [[nodiscard]] auto async_write(async_io::stream_socket_view socket,
                                   const_buffer buffer, int flags = 0) const;

    /**
     * Creates a sender for one socket write operation without retrying short
     * writes.
     */
    [[nodiscard]] auto async_write_some(async_io::stream_socket_view socket,
                                        const_buffer buffer,
                                        int flags = 0) const;

    [[nodiscard]] auto async_receive(async_io::datagram_socket_view socket,
                                     mutable_buffer buffer,
                                     int flags = 0) const;
    [[nodiscard]] auto async_send(async_io::datagram_socket_view socket,
                                  const_buffer buffer, int flags = 0) const;
    [[nodiscard]] auto async_receive_from(async_io::datagram_socket_view socket,
                                          mutable_buffer buffer,
                                          ip::endpoint& endpoint,
                                          int flags = 0) const;
    [[nodiscard]] auto async_send_to(async_io::datagram_socket_view socket,
                                     const_buffer buffer,
                                     const ip::endpoint& endpoint,
                                     int flags = 0) const;
    /**
     * Creates a sender for one descriptor read operation at an offset.
     */
    [[nodiscard]] auto async_read(async_io::descriptor_view descriptor,
                                  mutable_buffer buffer,
                                  std::uint64_t offset = 0) const;

    /**
     * Creates a sender for one descriptor read operation at an offset.
     */
    [[nodiscard]] auto async_read_some(async_io::descriptor_view descriptor,
                                       mutable_buffer buffer,
                                       std::uint64_t offset = 0) const;

    /**
     * Creates a sender that writes the whole buffer to a descriptor.
     */
    [[nodiscard]] auto async_write(async_io::descriptor_view descriptor,
                                   const_buffer buffer,
                                   std::uint64_t offset = 0) const;

    /**
     * Creates a sender for one descriptor write operation at an offset without
     * retrying short writes.
     */
    [[nodiscard]] auto async_write_some(async_io::descriptor_view descriptor,
                                        const_buffer buffer,
                                        std::uint64_t offset = 0) const;

    /**
     * Creates a sender that accepts one connection.
     */
    [[nodiscard]] auto async_accept(async_io::stream_socket_view socket,
                                    int flags = 0) const;

    /**
     * Creates a sender that connects a socket to an endpoint.
     */
    [[nodiscard]] auto async_connect(async_io::stream_socket_view socket,
                                     const ip::endpoint& endpoint) const;

    /**
     * Creates a sender that waits for descriptor events.
     */
    [[nodiscard]] auto async_poll(async_io::descriptor_view descriptor,
                                  unsigned poll_mask) const;

    /**
     * Creates a sender that resolves a DNS query into caller-provided storage.
     */
    [[nodiscard]] auto async_resolve(async_io::dns_query query,
                                     async_io::dns_result_view result) const;

    /**
     * Creates a sender that resolves a host and service into caller-provided
     * storage.
     */
    [[nodiscard]] auto async_resolve(std::string_view host,
                                     std::string_view service,
                                     async_io::dns_result_view result) const;

    /**
     * Compares whether two scheduler handles refer to the same context.
     */
    friend bool operator==(basic_scheduler lhs, basic_scheduler rhs) noexcept {
      return lhs.context_ == rhs.context_;
    }

   private:
    friend class io_context;

    explicit basic_scheduler(io_context& context) noexcept
        : context_(&context) {}

    io_context* context_;
  };

  /**
   * Scheduler with Asio dispatch-like schedule() semantics.
   */
  using dispatch_scheduler = basic_scheduler<schedule_kind::dispatch>;

  /**
   * Scheduler with Asio post-like schedule() semantics.
   */
  using post_scheduler = basic_scheduler<schedule_kind::post>;

  /**
   * Creates a context with default options.
   */
  io_context() noexcept;

  /**
   * Creates a context with explicit options.
   */
  explicit io_context(const io_context_options& options) noexcept;

  /** Releases worker, timer, and native context resources. */
  ~io_context() noexcept;

  /**
   * Copy construction is disabled because the context owns an io_uring context
   * and synchronization resources.
   */
  io_context(const io_context&) = delete;

  /**
   * Copy assignment is disabled because the context owns an io_uring context
   * and synchronization resources.
   */
  io_context& operator=(const io_context&) = delete;

  /**
   * Move construction is disabled because the context owns synchronization
   * resources and timer state.
   */
  io_context(io_context&&) = delete;

  /**
   * Move assignment is disabled because the context owns synchronization
   * resources and timer state.
   */
  io_context& operator=(io_context&&) = delete;

  /**
   * Returns whether the underlying native context is open.
   */
  [[nodiscard]] bool is_open() const noexcept;

  /**
   * Runs the context event loop.
   */
  void run() noexcept;

  /**
   * Requests the context event loop to stop.
   */
  int stop() noexcept;

  /**
   * Returns whether the current thread is running this context.
   */
  [[nodiscard]] bool is_in_context() const noexcept;

  /**
   * Returns a scheduler whose schedule() may complete inline on the context
   * thread.
   */
  [[nodiscard]] dispatch_scheduler get_dispatch_scheduler() noexcept;

  /**
   * Returns a scheduler whose schedule() always posts through the context loop.
   */
  [[nodiscard]] post_scheduler get_post_scheduler() noexcept;

 private:
  friend class steady_timer;
  friend class detail::timer_operation_base;
  friend class detail::timer_wakeup_operation;
  friend class detail::timer_update_operation;
  friend class detail::timer_driver_operation;
  template <class Receiver>
  friend class detail::timer_wait_operation;
  template <class Model, class Receiver>
  friend class detail::native_io_operation;

  /**
   * Returns the underlying Linux native async I/O context.
   */
  [[nodiscard]] async_io::linux_native::io_uring_context&
  native_context() noexcept {
    return select_native_context();
  }

  /**
   * Selects a native context for newly started work.
   */
  [[nodiscard]] async_io::linux_native::io_uring_context&
  select_native_context() noexcept;

  /**
   * Returns the primary native context used for timers and bootstrap work.
   */
  [[nodiscard]] async_io::linux_native::io_uring_context&
  primary_native_context() noexcept {
    return native_.context;
  }

  /**
   * Creates a sender that reads bytes from a non-owning stream socket
   * view and completes with bytes transferred.
   */
  [[nodiscard]] auto async_read(async_io::stream_socket_view socket,
                                mutable_buffer buffer, int flags = 0);

  [[nodiscard]] auto async_read_some(async_io::stream_socket_view socket,
                                     mutable_buffer buffer, int flags = 0);

  /**
   * Creates a sender that writes the whole buffer through a non-owning
   * stream socket view.
   */
  [[nodiscard]] auto async_write(async_io::stream_socket_view socket,
                                 const_buffer buffer, int flags = 0);

  /**
   * Creates a sender for one write operation through a non-owning
   * stream socket view.
   */
  [[nodiscard]] auto async_write_some(async_io::stream_socket_view socket,
                                      const_buffer buffer, int flags = 0);

  /**
   * Creates a sender that reads bytes from a file descriptor.
   */
  [[nodiscard]] auto async_read(async_io::descriptor_view descriptor,
                                mutable_buffer buffer,
                                std::uint64_t offset = 0);

  [[nodiscard]] auto async_read_some(async_io::descriptor_view descriptor,
                                     mutable_buffer buffer,
                                     std::uint64_t offset = 0);

  /**
   * Creates a sender that writes the whole buffer to a file descriptor.
   */
  [[nodiscard]] auto async_write(async_io::descriptor_view descriptor,
                                 const_buffer buffer, std::uint64_t offset = 0);

  /**
   * Creates a sender for one write operation to a file descriptor.
   */
  [[nodiscard]] auto async_write_some(async_io::descriptor_view descriptor,
                                      const_buffer buffer,
                                      std::uint64_t offset = 0);

  [[nodiscard]] auto async_receive(async_io::datagram_socket_view socket,
                                   mutable_buffer buffer, int flags = 0);
  [[nodiscard]] auto async_send(async_io::datagram_socket_view socket,
                                const_buffer buffer, int flags = 0);
  [[nodiscard]] auto async_receive_from(async_io::datagram_socket_view socket,
                                        mutable_buffer buffer,
                                        ip::endpoint& endpoint, int flags = 0);
  [[nodiscard]] auto async_send_to(async_io::datagram_socket_view socket,
                                   const_buffer buffer,
                                   const ip::endpoint& endpoint, int flags = 0);
  /**
   * Creates a sender that accepts one connection from a non-owning
   * listening socket view.
   */
  [[nodiscard]] auto async_accept(async_io::stream_socket_view socket,
                                  int flags = 0);

  /**
   * Creates a sender that connects a non-owning stream socket view.
   */
  [[nodiscard]] auto async_connect(async_io::stream_socket_view socket,
                                   const ip::endpoint& endpoint);

  /**
   * Creates a sender that waits for events on a file descriptor.
   */
  [[nodiscard]] auto async_poll(async_io::descriptor_view descriptor,
                                unsigned poll_mask);

  /**
   * Creates a sender that resolves a DNS query into caller-provided result
   * storage on the context run loop.
   */
  [[nodiscard]] auto async_resolve(async_io::dns_query query,
                                   async_io::dns_result_view result);

  /**
   * Creates a sender that resolves a host and service into caller-provided
   * result storage on the context run loop.
   */
  [[nodiscard]] auto async_resolve(std::string_view host,
                                   std::string_view service,
                                   async_io::dns_result_view result);

  /**
   * Publishes an operation for passive io_uring submission by a worker.
   */
  void enqueue_io(operation_base& operation) noexcept;

  /**
   * Posts an operation for execution on the context run loop.
   */
  void post(
      async_io::linux_native::io_uring_operation_base& operation) noexcept;

  [[nodiscard]] detail::native_worker& select_worker() noexcept;

  [[nodiscard]] detail::native_worker& select_io_worker() noexcept;

  [[nodiscard]] detail::native_worker* register_run_worker() noexcept;

  void submit_operations(
      operation_base* operations,
      async_io::linux_native::io_uring_context& native_context) noexcept;

  [[nodiscard]] static bool drain_pending_io(
      void* owner,
      async_io::linux_native::io_uring_context& native_context) noexcept;

  [[nodiscard]] operation_base* take_pending_io() noexcept;

  void wake_one_worker() noexcept;

  void register_timer(detail::timer_slot& timer) noexcept;

  void unregister_timer(detail::timer_slot& timer) noexcept;

  [[nodiscard]] std::size_t cancel_timer(detail::timer_slot& timer) noexcept;

  [[nodiscard]] std::size_t set_timer_expiry(detail::timer_slot& timer,
                                             time_point expiry) noexcept;

  [[nodiscard]] time_point timer_expiry(
      const detail::timer_slot& timer) const noexcept;

  void start_timer_wait(detail::timer_operation_base& operation,
                        detail::timer_slot& timer) noexcept;

  void on_timer_wakeup() noexcept;

  void on_timer_update() noexcept;

  void on_timer_driver() noexcept;

  void post_timer_driver() noexcept;

  [[nodiscard]] std::size_t drain_timer_submissions_locked(
      detail::timer_slot& timer) noexcept;

  [[nodiscard]] detail::timer_operation_base* take_timer_waiters_locked(
      detail::timer_slot& timer) noexcept;

  void push_timer_operation(std::atomic<detail::timer_operation_base*>& head,
                            detail::timer_operation_base& operation) noexcept;

  [[nodiscard]] static detail::timer_operation_base* reverse_timer_operations(
      detail::timer_operation_base* operations) noexcept;

  [[nodiscard]] static std::size_t count_timer_operations(
      detail::timer_operation_base* operations) noexcept;

  void post_timer_operations(detail::timer_operation_base* operations,
                             detail::timer_completion_kind completion) noexcept;

  void schedule_timer_wakeup_locked() noexcept;

  void submit_timer_wakeup_locked(time_point deadline) noexcept;

  void submit_timer_update_locked(time_point deadline) noexcept;

  async_io::linux_native::io_uring_task_queue_state task_queue_;
  detail::native_context_state native_;
  detail::timer_wakeup_operation timer_wakeup_operation_;
  detail::timer_update_operation timer_update_operation_;
  detail::timer_driver_operation timer_driver_operation_;

  detail::native_worker_state native_workers_;
  std::atomic_bool stop_requested_{false};

  detail::timer_state_data timers_;

  static thread_local detail::native_worker* current_native_worker_;
};

}  // namespace bupp

#include <bupp/linux/detail/io_context_native_io.h>
#include <bupp/linux/detail/io_context_state/native_worker.h>

#endif  // BUPP_LINUX_IO_CONTEXT_H_
