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
  struct native_worker;

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
     * Intrusive next pointer used by queued-I/O stacks.
     */
    operation_base* pending_next = nullptr;

    /**
     * Prepares the native operation for submission.
     */
    [[nodiscard]] virtual int prepare_for_submit() noexcept = 0;

    /**
     * Fills one native submission queue entry for this operation.
     */
    virtual void prepare(base::submission_queue_entry& sqe) noexcept = 0;

    /**
     * Completes the operation when submission preparation fails.
     */
    virtual void complete_submit_error(int result) noexcept = 0;

   private:
    friend class io_context;

    native_worker* native_worker_ = nullptr;
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
     * Submits all operations currently waiting in the queued I/O list.
     */
    [[nodiscard]] std::error_code flush_io_queue() const noexcept;

    /**
     * Returns the number of operations waiting in the queued I/O list.
     */
    [[nodiscard]] std::size_t queued_io_size() const noexcept;

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
     * Submits one prepared operation immediately through the owning context.
     */
    void submit_direct(operation_base& operation) const noexcept {
      context_->submit_direct(operation);
    }

    /**
     * Queues one operation for batched io_uring submission.
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
     * Creates a queued sender for one socket read operation.
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
     * Creates a direct-submission sender for one socket read operation.
     */
    [[nodiscard]] auto async_read_direct(async_io::stream_socket_view socket,
                                         mutable_buffer buffer,
                                         int flags = 0) const;

    /**
     * Creates a sender for one direct-submission socket read operation.
     */
    [[nodiscard]] auto async_read_some_direct(
        async_io::stream_socket_view socket, mutable_buffer buffer,
        int flags = 0) const;

    /**
     * Creates a queued sender that writes the whole buffer to a socket.
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

    /**
     * Creates a direct-submission sender that writes the whole buffer to a
     * socket.
     */
    [[nodiscard]] auto async_write_direct(async_io::stream_socket_view socket,
                                          const_buffer buffer,
                                          int flags = 0) const;

    /**
     * Creates a sender for one direct-submission socket write operation
     * without retrying short writes.
     */
    [[nodiscard]] auto async_write_some_direct(
        async_io::stream_socket_view socket, const_buffer buffer,
        int flags = 0) const;

    [[nodiscard]] auto async_receive(async_io::datagram_socket_view socket,
                                     mutable_buffer buffer,
                                     int flags = 0) const;
    [[nodiscard]] auto async_receive_direct(
        async_io::datagram_socket_view socket, mutable_buffer buffer,
        int flags = 0) const;
    [[nodiscard]] auto async_send(async_io::datagram_socket_view socket,
                                  const_buffer buffer, int flags = 0) const;
    [[nodiscard]] auto async_send_direct(async_io::datagram_socket_view socket,
                                         const_buffer buffer,
                                         int flags = 0) const;
    [[nodiscard]] auto async_receive_from(async_io::datagram_socket_view socket,
                                          mutable_buffer buffer,
                                          ip::endpoint& endpoint,
                                          int flags = 0) const;
    [[nodiscard]] auto async_receive_from_direct(
        async_io::datagram_socket_view socket, mutable_buffer buffer,
        ip::endpoint& endpoint, int flags = 0) const;
    [[nodiscard]] auto async_send_to(async_io::datagram_socket_view socket,
                                     const_buffer buffer,
                                     const ip::endpoint& endpoint,
                                     int flags = 0) const;
    [[nodiscard]] auto async_send_to_direct(
        async_io::datagram_socket_view socket, const_buffer buffer,
        const ip::endpoint& endpoint, int flags = 0) const;

    /**
     * Creates a queued sender for one descriptor read operation at an offset.
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
     * Creates a direct-submission sender for one descriptor read operation at
     * an offset.
     */
    [[nodiscard]] auto async_read_direct(async_io::descriptor_view descriptor,
                                         mutable_buffer buffer,
                                         std::uint64_t offset = 0) const;

    /**
     * Creates a sender for one direct-submission descriptor read operation at
     * an offset.
     */
    [[nodiscard]] auto async_read_some_direct(
        async_io::descriptor_view descriptor, mutable_buffer buffer,
        std::uint64_t offset = 0) const;

    /**
     * Creates a queued sender that writes the whole buffer to a descriptor.
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
     * Creates a direct-submission sender that writes the whole buffer to a
     * descriptor.
     */
    [[nodiscard]] auto async_write_direct(async_io::descriptor_view descriptor,
                                          const_buffer buffer,
                                          std::uint64_t offset = 0) const;

    /**
     * Creates a sender for one direct-submission descriptor write operation at
     * an offset without retrying short writes.
     */
    [[nodiscard]] auto async_write_some_direct(
        async_io::descriptor_view descriptor, const_buffer buffer,
        std::uint64_t offset = 0) const;

    /**
     * Creates a queued sender that accepts one connection.
     */
    [[nodiscard]] auto async_accept(async_io::stream_socket_view socket,
                                    int flags = 0) const;

    /**
     * Creates a direct-submission sender that accepts one connection.
     */
    [[nodiscard]] auto async_accept_direct(async_io::stream_socket_view socket,
                                           int flags = 0) const;

    /**
     * Creates a queued sender that connects a socket to an endpoint.
     */
    [[nodiscard]] auto async_connect(async_io::stream_socket_view socket,
                                     const ip::endpoint& endpoint) const;

    /**
     * Creates a direct-submission sender that connects a socket to an
     * endpoint.
     */
    [[nodiscard]] auto async_connect_direct(async_io::stream_socket_view socket,
                                            const ip::endpoint& endpoint) const;

    /**
     * Creates a queued sender that waits for descriptor events.
     */
    [[nodiscard]] auto async_poll(async_io::descriptor_view descriptor,
                                  unsigned poll_mask) const;

    /**
     * Creates a direct-submission sender that waits for descriptor events.
     */
    [[nodiscard]] auto async_poll_direct(async_io::descriptor_view descriptor,
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

  /**
   * Stops the flush timer and releases context resources.
   */
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
  template <class Receiver>
  friend class detail::timer_wait_operation;
  template <class Model, class Receiver>
  friend class detail::native_io_operation;

  /**
   * Submits all operations currently waiting in the queued I/O list.
   */
  [[nodiscard]] std::error_code flush_io_queue() noexcept;

  /**
   * Submits queued I/O, optionally waiting for exclusive io_uring access.
   */
  [[nodiscard]] std::error_code flush_io_queue(bool wait_for_gate) noexcept;

  /**
   * Returns the number of operations waiting in the queued I/O list.
   */
  [[nodiscard]] std::size_t queued_io_size() const noexcept;

  /**
   * Returns the underlying Linux native async I/O context.
   */
  [[nodiscard]] async_io::linux_native::io_uring_context&
  native_context() noexcept {
    return select_native_context();
  }

  /**
   * Returns the underlying Linux native async I/O context.
   */
  [[nodiscard]] const async_io::linux_native::io_uring_context& native_context()
      const noexcept {
    return native_context_;
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
    return native_context_;
  }

  /**
   * Creates a queued sender that reads bytes from a non-owning stream socket
   * view and completes with bytes transferred.
   */
  [[nodiscard]] auto async_read(async_io::stream_socket_view socket,
                                mutable_buffer buffer, int flags = 0);

  [[nodiscard]] auto async_read_some(async_io::stream_socket_view socket,
                                     mutable_buffer buffer, int flags = 0);

  /**
   * Creates a direct-submission sender that reads bytes from a non-owning
   * stream socket view and completes with bytes transferred.
   */
  [[nodiscard]] auto async_read_direct(async_io::stream_socket_view socket,
                                       mutable_buffer buffer, int flags = 0);

  [[nodiscard]] auto async_read_some_direct(async_io::stream_socket_view socket,
                                            mutable_buffer buffer,
                                            int flags = 0);

  /**
   * Creates a queued sender that writes the whole buffer through a non-owning
   * stream socket view.
   */
  [[nodiscard]] auto async_write(async_io::stream_socket_view socket,
                                 const_buffer buffer, int flags = 0);

  /**
   * Creates a queued sender for one write operation through a non-owning
   * stream socket view.
   */
  [[nodiscard]] auto async_write_some(async_io::stream_socket_view socket,
                                      const_buffer buffer, int flags = 0);

  /**
   * Creates a direct-submission sender that writes the whole buffer through a
   * non-owning stream socket view.
   */
  [[nodiscard]] auto async_write_direct(async_io::stream_socket_view socket,
                                        const_buffer buffer, int flags = 0);

  /**
   * Creates a direct-submission sender for one write operation through a
   * non-owning stream socket view.
   */
  [[nodiscard]] auto async_write_some_direct(
      async_io::stream_socket_view socket, const_buffer buffer, int flags = 0);

  /**
   * Creates a queued sender that reads bytes from a file descriptor.
   */
  [[nodiscard]] auto async_read(async_io::descriptor_view descriptor,
                                mutable_buffer buffer,
                                std::uint64_t offset = 0);

  [[nodiscard]] auto async_read_some(async_io::descriptor_view descriptor,
                                     mutable_buffer buffer,
                                     std::uint64_t offset = 0);

  /**
   * Creates a direct-submission sender that reads bytes from a file descriptor.
   */
  [[nodiscard]] auto async_read_direct(async_io::descriptor_view descriptor,
                                       mutable_buffer buffer,
                                       std::uint64_t offset = 0);

  [[nodiscard]] auto async_read_some_direct(
      async_io::descriptor_view descriptor, mutable_buffer buffer,
      std::uint64_t offset = 0);

  /**
   * Creates a queued sender that writes the whole buffer to a file descriptor.
   */
  [[nodiscard]] auto async_write(async_io::descriptor_view descriptor,
                                 const_buffer buffer, std::uint64_t offset = 0);

  /**
   * Creates a queued sender for one write operation to a file descriptor.
   */
  [[nodiscard]] auto async_write_some(async_io::descriptor_view descriptor,
                                      const_buffer buffer,
                                      std::uint64_t offset = 0);

  /**
   * Creates a direct-submission sender that writes the whole buffer to a file
   * descriptor.
   */
  [[nodiscard]] auto async_write_direct(async_io::descriptor_view descriptor,
                                        const_buffer buffer,
                                        std::uint64_t offset = 0);

  /**
   * Creates a direct-submission sender for one write operation to a file
   * descriptor.
   */
  [[nodiscard]] auto async_write_some_direct(
      async_io::descriptor_view descriptor, const_buffer buffer,
      std::uint64_t offset = 0);

  [[nodiscard]] auto async_receive(async_io::datagram_socket_view socket,
                                   mutable_buffer buffer, int flags = 0);
  [[nodiscard]] auto async_receive_direct(async_io::datagram_socket_view socket,
                                          mutable_buffer buffer, int flags = 0);
  [[nodiscard]] auto async_send(async_io::datagram_socket_view socket,
                                const_buffer buffer, int flags = 0);
  [[nodiscard]] auto async_send_direct(async_io::datagram_socket_view socket,
                                       const_buffer buffer, int flags = 0);
  [[nodiscard]] auto async_receive_from(async_io::datagram_socket_view socket,
                                        mutable_buffer buffer,
                                        ip::endpoint& endpoint, int flags = 0);
  [[nodiscard]] auto async_receive_from_direct(
      async_io::datagram_socket_view socket, mutable_buffer buffer,
      ip::endpoint& endpoint, int flags = 0);
  [[nodiscard]] auto async_send_to(async_io::datagram_socket_view socket,
                                   const_buffer buffer,
                                   const ip::endpoint& endpoint, int flags = 0);
  [[nodiscard]] auto async_send_to_direct(async_io::datagram_socket_view socket,
                                          const_buffer buffer,
                                          const ip::endpoint& endpoint,
                                          int flags = 0);

  /**
   * Creates a queued sender that accepts one connection from a non-owning
   * listening socket view.
   */
  [[nodiscard]] auto async_accept(async_io::stream_socket_view socket,
                                  int flags = 0);

  /**
   * Creates a direct-submission sender that accepts one connection from a
   * non-owning listening socket view.
   */
  [[nodiscard]] auto async_accept_direct(async_io::stream_socket_view socket,
                                         int flags = 0);

  /**
   * Creates a queued sender that connects a non-owning stream socket view.
   */
  [[nodiscard]] auto async_connect(async_io::stream_socket_view socket,
                                   const ip::endpoint& endpoint);

  /**
   * Creates a direct-submission sender that connects a non-owning stream
   * socket view.
   */
  [[nodiscard]] auto async_connect_direct(async_io::stream_socket_view socket,
                                          const ip::endpoint& endpoint);

  /**
   * Creates a queued sender that waits for events on a file descriptor.
   */
  [[nodiscard]] auto async_poll(async_io::descriptor_view descriptor,
                                unsigned poll_mask);

  /**
   * Creates a direct-submission sender that waits for descriptor events.
   */
  [[nodiscard]] auto async_poll_direct(async_io::descriptor_view descriptor,
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
   * Queues an operation for batched io_uring submission.
   */
  void enqueue_io(operation_base& operation) noexcept;

  /**
   * Submits an operation immediately through the native context.
   */
  void submit_direct(operation_base& operation) noexcept;

  /**
   * Posts an operation for execution on the context run loop.
   */
  void post(
      async_io::linux_native::io_uring_operation_base& operation) noexcept;

  class timer_wakeup_operation
      : public async_io::linux_native::io_uring_operation_base {
   public:
    explicit timer_wakeup_operation(io_context& context) noexcept;

    void set_timeout(duration timeout) noexcept;

    void prepare(base::submission_queue_entry& sqe) noexcept;

    void execute() noexcept override;

   private:
    io_context* context_;
    async_io::linux_native::detail::timeout_request timeout_;
  };

  class timer_update_operation
      : public async_io::linux_native::io_uring_operation_base {
   public:
    explicit timer_update_operation(io_context& context) noexcept;

    void set_timeout(duration timeout) noexcept;

    void prepare(base::submission_queue_entry& sqe) noexcept;

    void execute() noexcept override;

   private:
    io_context* context_;
    async_io::linux_native::detail::timeout_request timeout_;
  };

  class timer_driver_operation
      : public async_io::linux_native::io_uring_operation_base {
   public:
    explicit timer_driver_operation(io_context& context) noexcept;

    void execute() noexcept override;

   private:
    io_context* context_;
  };

  class queued_io_flush_operation : public detail::timer_operation_base {
   public:
    explicit queued_io_flush_operation(io_context& context) noexcept;

    void execute() noexcept override;
  };

  struct timer_state_data {
    enum class queued_operation_state {
      idle,
      posted,
    };

    enum class timeout_state {
      idle,
      armed,
      updating,
      update_pending,
    };

    [[nodiscard]] bool queue_driver() noexcept;

    void complete_driver() noexcept;

    [[nodiscard]] bool queue_flush_wait() noexcept;

    void complete_flush_wait() noexcept;

    [[nodiscard]] bool can_submit_wakeup() const noexcept;

    [[nodiscard]] bool can_submit_update(time_point deadline) const noexcept;

    void complete_wakeup() noexcept;

    void complete_update() noexcept;

    void mark_wakeup_submitted(time_point deadline) noexcept;

    void mark_update_submitted(time_point deadline) noexcept;

    void push_heap(detail::timer_heap_item item) noexcept;

    void pop_heap() noexcept;

    void swap_heap_items(std::size_t first, std::size_t second) noexcept;

    void sift_heap_up(std::size_t index) noexcept;

    void sift_heap_down(std::size_t index) noexcept;

    [[nodiscard]] bool heap_item_less(std::size_t first,
                                      std::size_t second) const noexcept;

    mutable std::mutex mutex;
    bexec::detail::manual_lifetime<queued_io_flush_operation>
        queued_io_flush_wait;
    std::unordered_map<std::uint64_t, detail::timer_slot*> timers;
    std::vector<detail::timer_heap_item> heap;
    std::uint64_t next_timer_id = 1;
    queued_operation_state queued_io_flush = queued_operation_state::idle;
    queued_operation_state driver = queued_operation_state::idle;
    timeout_state timeout = timeout_state::idle;
    time_point armed_deadline{};
  };

  struct native_worker {
    explicit native_worker(io_context& owner) noexcept : owner(&owner) {}

    io_context* owner = nullptr;
    std::atomic<native_worker*> next{nullptr};
    std::atomic<async_io::linux_native::io_uring_context*> context{nullptr};
    std::unique_ptr<async_io::linux_native::io_uring_context> owned_context;
    std::atomic<operation_base*> pending_io_head{nullptr};
  };

  [[nodiscard]] native_worker& primary_worker() noexcept;

  [[nodiscard]] native_worker& select_worker() noexcept;

  [[nodiscard]] native_worker& select_io_worker() noexcept;

  [[nodiscard]] native_worker& ensure_operation_worker(
      operation_base& operation) noexcept;

  [[nodiscard]] native_worker* register_run_worker() noexcept;

  [[nodiscard]] std::error_code flush_operations(
      operation_base* operations,
      async_io::linux_native::io_uring_context& native_context,
      async_io::linux_native::io_uring_context::uring_lock& lock) noexcept;

  [[nodiscard]] std::error_code flush_io_queue(native_worker& worker,
                                               bool wait_for_gate) noexcept;

  [[nodiscard]] operation_base* take_pending_io(native_worker& worker) noexcept;

  void move_global_io_to_worker(native_worker& worker) noexcept;

  [[nodiscard]] operation_base* take_worker_pending_io(
      native_worker& worker) noexcept;

  void arm_flush_timer() noexcept;

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

  void on_queued_io_flush(detail::timer_completion_kind completion) noexcept;

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

  async_io::linux_native::io_uring_context native_context_;
  linux_io_context_options linux_options_{};
  timer_wakeup_operation timer_wakeup_operation_;
  timer_update_operation timer_update_operation_;
  timer_driver_operation timer_driver_operation_;

  std::atomic<std::size_t> pending_io_count_{0};
  std::atomic<operation_base*> global_pending_io_head_{nullptr};
  native_worker* native_workers_head_ = nullptr;
  std::atomic<native_worker*> round_robin_cursor_{nullptr};
  std::atomic<std::size_t> active_native_worker_count_{1};
  std::atomic<std::size_t> next_run_worker_{0};
  std::atomic_bool stop_requested_{false};

  timer_state_data timers_;
  steady_timer queued_io_flush_timer_;

  static thread_local native_worker* current_native_worker_;
};

/** @cond BUPP_DETAIL */
namespace detail {

[[nodiscard]] inline std::error_code errno_result(int result) noexcept {
  return std::error_code(-result, std::generic_category());
}

template <class Receiver>
[[nodiscard]] bool stop_requested(const Receiver& receiver) noexcept {
  auto env = bexec::get_env(receiver);
  auto token = bexec::query(env, bexec::get_stop_token);
  return token.stop_requested();
}

template <class Model>
concept has_immediate_io = requires(Model& model) {
  { model.try_immediate() } -> std::convertible_to<int>;
};

[[nodiscard]] inline bool should_wait_for_immediate_result(
    int result) noexcept {
  return result == -EAGAIN || result == -EWOULDBLOCK;
}

[[nodiscard]] inline int immediate_socket_result(ssize_t result) noexcept {
  if (result >= 0) {
    return static_cast<int>(result);
  }
  const int error = errno;
  if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK) {
    return -EAGAIN;
  }
  return -error;
}

[[nodiscard]] inline bool should_defer_nowait_read_error(int error) noexcept {
  return error == ENOSYS || error == EOPNOTSUPP || error == EINVAL;
}

[[nodiscard]] constexpr int nowait_read_flag() noexcept {
#ifdef RWF_NOWAIT
  return RWF_NOWAIT;
#else
  return 0x00000008;
#endif
}

[[nodiscard]] inline ssize_t pread_nowait(int descriptor, void* data,
                                          std::size_t size,
                                          std::uint64_t offset) noexcept {
#ifdef SYS_preadv2
  struct iovec view {
    data, size
  };
  const auto low = static_cast<unsigned long>(offset);
  unsigned long high = 0;
  if constexpr (sizeof(unsigned long) < sizeof(std::uint64_t)) {
    high = static_cast<unsigned long>(offset >> (sizeof(unsigned long) * 8U));
  }
  return ::syscall(SYS_preadv2, descriptor, &view, 1, low, high,
                   nowait_read_flag());
#else
  (void)descriptor;
  (void)data;
  (void)size;
  (void)offset;
  errno = ENOSYS;
  return -1;
#endif
}

template <class Model, class Receiver>
class native_io_operation : public io_context::operation_base {
 public:
  native_io_operation(io_context& context, Model model, submit_mode mode,
                      Receiver receiver)
      : context_(&context),
        model_(std::move(model)),
        mode_(mode),
        receiver_(std::move(receiver)) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    model_.prepare(sqe);
  }

  [[nodiscard]] int prepare_for_submit() noexcept override {
    completion_ = completion_kind::value;
    return context_->select_native_context().prepare(*this);
  }

  void complete_submit_error(int result) noexcept override {
    completion_ = completion_kind::error;
    error_ = errno_result(result);
  }

  void start() noexcept {
    if (stop_requested(receiver_)) {
      completion_ = completion_kind::stopped;
      context_->post(*this);
      return;
    }

    if (try_complete_immediate()) {
      return;
    }

    completion_ = completion_kind::value;
    if (mode_ == submit_mode::direct) {
      context_->submit_direct(*this);
    } else {
      context_->enqueue_io(*this);
    }
  }

  void execute() noexcept override {
    switch (completion_) {
      case completion_kind::value:
        if (model_.is_error_result(this->result)) {
          bexec::set_error(std::move(receiver_),
                           model_.make_error(this->result));
        } else {
          model_.set_value(std::move(receiver_), this->result, this->flags);
        }
        break;
      case completion_kind::error:
        bexec::set_error(std::move(receiver_), error_);
        break;
      case completion_kind::stopped:
        bexec::set_stopped(std::move(receiver_));
        break;
    }
  }

 private:
  [[nodiscard]] bool try_complete_immediate() noexcept {
    if constexpr (has_immediate_io<Model>) {
      const int result = model_.try_immediate();
      if (should_wait_for_immediate_result(result)) {
        return false;
      }

      this->result = result;
      this->flags = 0;
      if (result < 0) {
        completion_ = completion_kind::error;
        error_ = errno_result(result);
      } else {
        completion_ = completion_kind::value;
      }
      context_->post(*this);
      return true;
    } else {
      return false;
    }
  }

  enum class completion_kind {
    value,
    error,
    stopped,
  };

  io_context* context_;
  Model model_;
  submit_mode mode_;
  std::remove_cvref_t<Receiver> receiver_;
  completion_kind completion_ = completion_kind::value;
  std::error_code error_;
};

template <class Model>
class native_io_sender {
 public:
  using completion_signatures = typename Model::completion_signatures;

  native_io_sender(io_context& context, Model model, submit_mode mode) noexcept
      : context_(&context), model_(std::move(model)), mode_(mode) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return native_io_operation<Model, std::remove_cvref_t<Receiver>>(
        *context_, std::move(model_), mode_, std::move(receiver));
  }

  template <class Receiver>
    requires std::copy_constructible<Model>
  auto connect(Receiver receiver) const& {
    return native_io_operation<Model, std::remove_cvref_t<Receiver>>(
        *context_, model_, mode_, std::move(receiver));
  }

 private:
  io_context* context_;
  Model model_;
  submit_mode mode_;
};

class read_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  read_model(async_io::descriptor_view descriptor, mutable_buffer buffer,
             std::uint64_t offset) noexcept
      : descriptor_(descriptor), buffer_(buffer), offset_(offset) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_read(
        descriptor_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        offset_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result = pread_nowait(
        descriptor_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        offset_);
    if (result >= 0) {
      return static_cast<int>(result);
    }

    const int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK ||
        should_defer_nowait_read_error(error)) {
      return -EAGAIN;
    }
    return -error;
  }

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }

  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<std::size_t>(result));
  }

 private:
  async_io::descriptor_view descriptor_;
  mutable_buffer buffer_;
  std::uint64_t offset_;
};

class write_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  write_model(async_io::descriptor_view descriptor, const_buffer buffer,
              std::uint64_t offset) noexcept
      : descriptor_(descriptor), buffer_(buffer), offset_(offset) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_write(
        descriptor_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        offset_);
  }

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }

  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<std::size_t>(result));
  }

 private:
  async_io::descriptor_view descriptor_;
  const_buffer buffer_;
  std::uint64_t offset_;
};

class socket_read_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  socket_read_model(async_io::stream_socket_view socket, mutable_buffer buffer,
                    int flags) noexcept
      : socket_(socket), buffer_(buffer), flags_(flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    const async_io::buffer_view view = buffer_.view();
    sqe.prep_recv(socket_.native_handle(), view.data,
                  async_io::linux_native::detail::bounded_io_size(view.size),
                  flags_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const async_io::buffer_view view = buffer_.view();
    const ssize_t result =
        ::recv(socket_.native_handle(), view.data,
               async_io::linux_native::detail::bounded_io_size(view.size),
               flags_ | MSG_DONTWAIT);
    return immediate_socket_result(result);
  }

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }

  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<std::size_t>(result));
  }

 private:
  async_io::stream_socket_view socket_;
  mutable_buffer buffer_;
  int flags_;
};

class socket_write_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  socket_write_model(async_io::stream_socket_view socket, const_buffer buffer,
                     int flags) noexcept
      : socket_(socket), buffer_(buffer), flags_(flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_send(
        socket_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result =
        ::send(socket_.native_handle(), buffer_.data(),
               async_io::linux_native::detail::bounded_io_size(buffer_.size()),
               flags_ | MSG_DONTWAIT);
    return immediate_socket_result(result);
  }

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }

  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<std::size_t>(result));
  }

 private:
  async_io::stream_socket_view socket_;
  const_buffer buffer_;
  int flags_;
};

class datagram_receive_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  datagram_receive_model(async_io::datagram_socket_view socket,
                         mutable_buffer buffer, int flags) noexcept
      : socket_(socket), buffer_(buffer), flags_(flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_recv(
        socket_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result =
        ::recv(socket_.native_handle(), buffer_.data(),
               async_io::linux_native::detail::bounded_io_size(buffer_.size()),
               flags_ | MSG_DONTWAIT);
    return immediate_socket_result(result);
  }

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }
  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }
  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<std::size_t>(result));
  }

 private:
  async_io::datagram_socket_view socket_;
  mutable_buffer buffer_;
  int flags_;
};

class datagram_send_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  datagram_send_model(async_io::datagram_socket_view socket,
                      const_buffer buffer, int flags) noexcept
      : socket_(socket), buffer_(buffer), flags_(flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_send(
        socket_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_);
  }
  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result =
        ::send(socket_.native_handle(), buffer_.data(),
               async_io::linux_native::detail::bounded_io_size(buffer_.size()),
               flags_ | MSG_DONTWAIT);
    return immediate_socket_result(result);
  }
  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }
  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }
  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<std::size_t>(result));
  }

 private:
  async_io::datagram_socket_view socket_;
  const_buffer buffer_;
  int flags_;
};

class datagram_receive_from_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  datagram_receive_from_model(async_io::datagram_socket_view socket,
                              mutable_buffer buffer, ip::endpoint& endpoint,
                              int flags) noexcept
      : socket_(socket), buffer_(buffer), endpoint_(&endpoint), flags_(flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    remote_address_ = {};
    buffer_entry_ = {
        buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size())};
    message_ = {};
    message_.msg_name = &remote_address_;
    message_.msg_namelen = sizeof(remote_address_);
    message_.msg_iov = &buffer_entry_;
    message_.msg_iovlen = 1;
    sqe.prep_recvmsg(socket_.native_handle(), &message_,
                     static_cast<unsigned>(flags_));
  }

  [[nodiscard]] int try_immediate() noexcept {
    remote_address_ = {};
    socklen_t size = sizeof(remote_address_);
    const ssize_t result = ::recvfrom(
        socket_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_ | MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&remote_address_),
        &size);
    if (result >= 0) {
      message_.msg_namelen = size;
    }
    return immediate_socket_result(result);
  }

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }
  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    const auto endpoint = async_io::linux_native::make_endpoint(
        reinterpret_cast<const sockaddr*>(&remote_address_),
        message_.msg_namelen);
    if (!endpoint.has_value()) {
      endpoint_->reset();
      bexec::set_error(
          std::forward<Receiver>(receiver),
          std::make_error_code(std::errc::address_family_not_supported));
      return;
    }
    *endpoint_ = *endpoint;
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<std::size_t>(result));
  }

 private:
  async_io::datagram_socket_view socket_;
  mutable_buffer buffer_;
  ip::endpoint* endpoint_;
  sockaddr_storage remote_address_{};
  iovec buffer_entry_{};
  msghdr message_{};
  int flags_;
};

class datagram_send_to_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  datagram_send_to_model(async_io::datagram_socket_view socket,
                         const_buffer buffer, const ip::endpoint& endpoint,
                         int flags)
      : socket_(socket),
        buffer_(buffer),
        remote_address_(endpoint),
        flags_(flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    buffer_entry_ = {
        const_cast<void*>(buffer_.data()),
        async_io::linux_native::detail::bounded_io_size(buffer_.size())};
    message_ = {};
    message_.msg_name = const_cast<sockaddr*>(remote_address_.data());
    message_.msg_namelen = remote_address_.size();
    message_.msg_iov = &buffer_entry_;
    message_.msg_iovlen = 1;
    sqe.prep_sendmsg(socket_.native_handle(), &message_,
                     static_cast<unsigned>(flags_));
  }
  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result = ::sendto(
        socket_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_ | MSG_DONTWAIT, remote_address_.data(), remote_address_.size());
    return immediate_socket_result(result);
  }
  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }
  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }
  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<std::size_t>(result));
  }

 private:
  async_io::datagram_socket_view socket_;
  const_buffer buffer_;
  async_io::linux_native::socket_address remote_address_;
  iovec buffer_entry_{};
  msghdr message_{};
  int flags_;
};

class accept_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(int),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  accept_model(async_io::stream_socket_view socket, int flags) noexcept
      : socket_(socket), flags_(flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_accept(socket_.native_handle(), nullptr, nullptr, flags_);
  }

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }

  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), result);
  }

 private:
  async_io::stream_socket_view socket_;
  int flags_;
};

class connect_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  connect_model(async_io::stream_socket_view socket,
                const ip::endpoint& endpoint)
      : socket_(socket), address_(endpoint) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_connect(socket_.native_handle(), address_.data(), address_.size());
  }

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }

  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver));
  }

 private:
  async_io::stream_socket_view socket_;
  async_io::linux_native::socket_address address_;
};

class poll_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(unsigned),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  poll_model(async_io::descriptor_view descriptor, unsigned poll_mask) noexcept
      : request_(descriptor, poll_mask) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    request_.prepare(sqe);
  }

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }

  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<unsigned>(result));
  }

 private:
  async_io::linux_native::io_uring_poll_request request_;
};

template <class Receiver>
class timer_wait_operation : public timer_operation_base {
 public:
  timer_wait_operation(steady_timer& timer, Receiver receiver)
      : timer_operation_base(timer.context()),
        timer_(&timer.timer_),
        receiver_(std::move(receiver)) {}

  void start() noexcept {
    if (stop_requested(receiver_)) {
      this->timer_completion_ = timer_completion_kind::stopped;
      (void)this->timer_context_->native_context().post(*this);
      return;
    }

    this->timer_context_->start_timer_wait(*this, *timer_);
  }

  void execute() noexcept override {
    const timer_completion_kind completion = this->timer_completion();
    if (completion == timer_completion_kind::stopped) {
      bexec::set_stopped(std::move(receiver_));
    } else {
      bexec::set_value(std::move(receiver_));
    }
  }

 private:
  detail::timer_slot* timer_;
  std::remove_cvref_t<Receiver> receiver_;
};

class timer_wait_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  explicit timer_wait_sender(steady_timer& timer) noexcept : timer_(&timer) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return timer_wait_operation<std::remove_cvref_t<Receiver>>(
        *timer_, std::move(receiver));
  }

  template <class Receiver>
  auto connect(Receiver receiver) const& {
    return timer_wait_operation<std::remove_cvref_t<Receiver>>(
        *timer_, std::move(receiver));
  }

 private:
  steady_timer* timer_;
};

class socket_write_all_state {
 public:
  socket_write_all_state(io_context& context,
                         async_io::stream_socket_view socket,
                         const_buffer buffer, int flags,
                         submit_mode mode) noexcept
      : context(&context),
        socket(socket),
        buffer(buffer),
        flags(flags),
        mode(mode) {}

  [[nodiscard]] std::size_t remaining() const noexcept {
    return buffer.size() - transferred;
  }

  [[nodiscard]] bool empty() const noexcept { return buffer.size() == 0; }

  [[nodiscard]] const_buffer current_buffer() const noexcept {
    const auto* data = static_cast<const char*>(buffer.data());
    return const_buffer(data + transferred, remaining());
  }

  [[nodiscard]] auto make_sender() noexcept {
    return native_io_sender<socket_write_model>(
        *context, socket_write_model(socket, current_buffer(), flags), mode);
  }

  void advance(std::size_t bytes) noexcept {
    transferred += bytes;
    if (transferred >= buffer.size()) {
      done = true;
    }
  }

  io_context* context;
  async_io::stream_socket_view socket;
  const_buffer buffer;
  int flags;
  submit_mode mode;
  std::size_t transferred = 0;
  bool done = false;
};

class descriptor_write_all_state {
 public:
  descriptor_write_all_state(io_context& context,
                             async_io::descriptor_view descriptor,
                             const_buffer buffer, std::uint64_t offset,
                             submit_mode mode) noexcept
      : context(&context),
        descriptor(descriptor),
        buffer(buffer),
        offset(offset),
        mode(mode) {}

  [[nodiscard]] std::size_t remaining() const noexcept {
    return buffer.size() - transferred;
  }

  [[nodiscard]] bool empty() const noexcept { return buffer.size() == 0; }

  [[nodiscard]] const_buffer current_buffer() const noexcept {
    const auto* data = static_cast<const char*>(buffer.data());
    return const_buffer(data + transferred, remaining());
  }

  [[nodiscard]] auto make_sender() noexcept {
    return native_io_sender<write_model>(
        *context,
        write_model(descriptor, current_buffer(), offset + transferred), mode);
  }

  void advance(std::size_t bytes) noexcept {
    transferred += bytes;
    if (transferred >= buffer.size()) {
      done = true;
    }
  }

  io_context* context;
  async_io::descriptor_view descriptor;
  const_buffer buffer;
  std::uint64_t offset;
  submit_mode mode;
  std::size_t transferred = 0;
  bool done = false;
};

template <class State, class Receiver>
class write_all_step_operation {
 public:
  using receiver_type = std::remove_cvref_t<Receiver>;

  class child_receiver {
   public:
    explicit child_receiver(write_all_step_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] decltype(auto) get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::size_t bytes) noexcept {
      operation_->handle_value(bytes);
    }

    void set_error(std::error_code error) noexcept {
      operation_->complete_error(error);
    }

    void set_stopped() noexcept { operation_->complete_stopped(); }

   private:
    write_all_step_operation* operation_;
  };

  using child_sender_type = decltype(std::declval<State&>().make_sender());
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  write_all_step_operation(State* state, Receiver receiver)
      : state_(state), receiver_(std::move(receiver)) {}

  void start() noexcept {
    if (state_->remaining() == 0) {
      complete_value(0);
      return;
    }

    child_operation_.emplace_from([this] {
      return bexec::connect(state_->make_sender(), child_receiver(*this));
    });
    bexec::start(*child_operation_);
  }

 private:
  void handle_value(std::size_t bytes) noexcept {
    if (bytes == 0) {
      complete_error(std::make_error_code(std::errc::broken_pipe));
      return;
    }
    if (bytes > state_->remaining()) {
      complete_error(std::make_error_code(std::errc::protocol_error));
      return;
    }

    state_->advance(bytes);
    complete_value(bytes);
  }

  void complete_value(std::size_t bytes) noexcept {
    bexec::set_value(std::move(receiver_), bytes);
  }

  void complete_error(std::error_code error) noexcept {
    bexec::set_error(std::move(receiver_), error);
  }

  void complete_stopped() noexcept { bexec::set_stopped(std::move(receiver_)); }

  State* state_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

template <class State>
class write_all_step_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  explicit write_all_step_sender(State* state) noexcept : state_(state) {}

  template <class Receiver>
  auto connect(Receiver receiver) const {
    return write_all_step_operation<State, std::remove_cvref_t<Receiver>>(
        state_, std::move(receiver));
  }

 private:
  State* state_;
};

template <class State>
class write_all_step_factory {
 public:
  explicit write_all_step_factory(State* state) noexcept : state_(state) {}

  [[nodiscard]] auto operator()() const noexcept {
    return write_all_step_sender<State>(state_);
  }

 private:
  State* state_;
};

template <class State>
class write_all_done_predicate {
 public:
  explicit write_all_done_predicate(State* state) noexcept : state_(state) {}

  [[nodiscard]] bool operator()() const noexcept { return state_->done; }

 private:
  State* state_;
};

template <class State, class Receiver>
class write_all_operation {
 public:
  using receiver_type = std::remove_cvref_t<Receiver>;
  using factory_type = write_all_step_factory<State>;
  using predicate_type = write_all_done_predicate<State>;
  using repeat_sender_type = decltype(bexec::repeat_until(
      std::declval<factory_type>(), std::declval<predicate_type>()));

  class repeat_receiver {
   public:
    explicit repeat_receiver(write_all_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] decltype(auto) get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::size_t) noexcept { operation_->complete_value(); }

    template <class Error>
    void set_error(Error&& error) noexcept {
      operation_->complete_error(std::forward<Error>(error));
    }

    void set_stopped() noexcept { operation_->complete_stopped(); }

   private:
    write_all_operation* operation_;
  };

  using repeat_operation_type = decltype(bexec::connect(
      std::declval<repeat_sender_type>(), std::declval<repeat_receiver>()));

  write_all_operation(State state, Receiver receiver)
      : state_(std::move(state)), receiver_(std::move(receiver)) {
    repeat_operation_.emplace_from([this] {
      return bexec::connect(
          bexec::repeat_until(factory_type(&state_), predicate_type(&state_)),
          repeat_receiver(*this));
    });
  }

  write_all_operation(const write_all_operation&) = delete;
  write_all_operation& operator=(const write_all_operation&) = delete;
  write_all_operation(write_all_operation&&) = delete;
  write_all_operation& operator=(write_all_operation&&) = delete;

  void start() noexcept {
    if (stop_requested(receiver_)) {
      complete_stopped();
      return;
    }
    if (state_.empty()) {
      complete_value();
      return;
    }

    bexec::start(*repeat_operation_);
  }

 private:
  void complete_value() noexcept {
    bexec::set_value(std::move(receiver_), state_.transferred);
  }

  void complete_error(std::error_code error) noexcept {
    bexec::set_error(std::move(receiver_), error);
  }

  template <class Error>
  void complete_error(Error&&) noexcept {
    bexec::set_error(std::move(receiver_),
                     std::make_error_code(std::errc::protocol_error));
  }

  void complete_stopped() noexcept { bexec::set_stopped(std::move(receiver_)); }

  State state_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<repeat_operation_type> repeat_operation_;
};

template <class State>
class write_all_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  explicit write_all_sender(State state) noexcept : state_(std::move(state)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return write_all_operation<State, std::remove_cvref_t<Receiver>>(
        std::move(state_), std::move(receiver));
  }

  template <class Receiver>
  auto connect(Receiver receiver) const& {
    return write_all_operation<State, std::remove_cvref_t<Receiver>>(
        state_, std::move(receiver));
  }

 private:
  State state_;
};

}  // namespace detail
/** @endcond */

inline auto io_context::async_read(async_io::stream_socket_view socket,
                                   mutable_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::socket_read_model(socket, buffer, flags),
      submit_mode::queued);
}

inline auto io_context::async_read_some(async_io::stream_socket_view socket,
                                        mutable_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::socket_read_model(socket, buffer, flags),
      submit_mode::queued);
}

inline auto io_context::async_read_direct(async_io::stream_socket_view socket,
                                          mutable_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::socket_read_model(socket, buffer, flags),
      submit_mode::direct);
}

inline auto io_context::async_read_some_direct(
    async_io::stream_socket_view socket, mutable_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::socket_read_model(socket, buffer, flags),
      submit_mode::direct);
}

inline auto io_context::async_write(async_io::stream_socket_view socket,
                                    const_buffer buffer, int flags) {
  return detail::write_all_sender(detail::socket_write_all_state(
      *this, socket, buffer, flags, submit_mode::queued));
}

inline auto io_context::async_write_some(async_io::stream_socket_view socket,
                                         const_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::socket_write_model(socket, buffer, flags),
      submit_mode::queued);
}

inline auto io_context::async_write_direct(async_io::stream_socket_view socket,
                                           const_buffer buffer, int flags) {
  return detail::write_all_sender(detail::socket_write_all_state(
      *this, socket, buffer, flags, submit_mode::direct));
}

inline auto io_context::async_write_some_direct(
    async_io::stream_socket_view socket, const_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::socket_write_model(socket, buffer, flags),
      submit_mode::direct);
}

inline auto io_context::async_receive(async_io::datagram_socket_view socket,
                                      mutable_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::datagram_receive_model(socket, buffer, flags),
      submit_mode::queued);
}

inline auto io_context::async_receive_direct(
    async_io::datagram_socket_view socket, mutable_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::datagram_receive_model(socket, buffer, flags),
      submit_mode::direct);
}

inline auto io_context::async_send(async_io::datagram_socket_view socket,
                                   const_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::datagram_send_model(socket, buffer, flags),
      submit_mode::queued);
}

inline auto io_context::async_send_direct(async_io::datagram_socket_view socket,
                                          const_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::datagram_send_model(socket, buffer, flags),
      submit_mode::direct);
}

inline auto io_context::async_receive_from(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    ip::endpoint& endpoint, int flags) {
  return detail::native_io_sender(
      *this,
      detail::datagram_receive_from_model(socket, buffer, endpoint, flags),
      submit_mode::queued);
}

inline auto io_context::async_receive_from_direct(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    ip::endpoint& endpoint, int flags) {
  return detail::native_io_sender(
      *this,
      detail::datagram_receive_from_model(socket, buffer, endpoint, flags),
      submit_mode::direct);
}

inline auto io_context::async_send_to(async_io::datagram_socket_view socket,
                                      const_buffer buffer,
                                      const ip::endpoint& endpoint, int flags) {
  return detail::native_io_sender(
      *this, detail::datagram_send_to_model(socket, buffer, endpoint, flags),
      submit_mode::queued);
}

inline auto io_context::async_send_to_direct(
    async_io::datagram_socket_view socket, const_buffer buffer,
    const ip::endpoint& endpoint, int flags) {
  return detail::native_io_sender(
      *this, detail::datagram_send_to_model(socket, buffer, endpoint, flags),
      submit_mode::direct);
}

inline auto io_context::async_read(async_io::descriptor_view descriptor,
                                   mutable_buffer buffer,
                                   std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::read_model(descriptor, buffer, offset),
      submit_mode::queued);
}

inline auto io_context::async_read_some(async_io::descriptor_view descriptor,
                                        mutable_buffer buffer,
                                        std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::read_model(descriptor, buffer, offset),
      submit_mode::queued);
}

inline auto io_context::async_read_direct(async_io::descriptor_view descriptor,
                                          mutable_buffer buffer,
                                          std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::read_model(descriptor, buffer, offset),
      submit_mode::direct);
}

inline auto io_context::async_read_some_direct(
    async_io::descriptor_view descriptor, mutable_buffer buffer,
    std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::read_model(descriptor, buffer, offset),
      submit_mode::direct);
}

inline auto io_context::async_write(async_io::descriptor_view descriptor,
                                    const_buffer buffer, std::uint64_t offset) {
  return detail::write_all_sender(detail::descriptor_write_all_state(
      *this, descriptor, buffer, offset, submit_mode::queued));
}

inline auto io_context::async_write_some(async_io::descriptor_view descriptor,
                                         const_buffer buffer,
                                         std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::write_model(descriptor, buffer, offset),
      submit_mode::queued);
}

inline auto io_context::async_write_direct(async_io::descriptor_view descriptor,
                                           const_buffer buffer,
                                           std::uint64_t offset) {
  return detail::write_all_sender(detail::descriptor_write_all_state(
      *this, descriptor, buffer, offset, submit_mode::direct));
}

inline auto io_context::async_write_some_direct(
    async_io::descriptor_view descriptor, const_buffer buffer,
    std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::write_model(descriptor, buffer, offset),
      submit_mode::direct);
}

inline auto io_context::async_accept(async_io::stream_socket_view socket,
                                     int flags) {
  return detail::native_io_sender(*this, detail::accept_model(socket, flags),
                                  submit_mode::queued);
}

inline auto io_context::async_accept_direct(async_io::stream_socket_view socket,
                                            int flags) {
  return detail::native_io_sender(*this, detail::accept_model(socket, flags),
                                  submit_mode::direct);
}

inline auto io_context::async_connect(async_io::stream_socket_view socket,
                                      const ip::endpoint& endpoint) {
  return detail::native_io_sender(
      *this, detail::connect_model(socket, endpoint), submit_mode::queued);
}

inline auto io_context::async_connect_direct(
    async_io::stream_socket_view socket, const ip::endpoint& endpoint) {
  return detail::native_io_sender(
      *this, detail::connect_model(socket, endpoint), submit_mode::direct);
}

inline auto io_context::async_poll(async_io::descriptor_view descriptor,
                                   unsigned poll_mask) {
  return detail::native_io_sender(
      *this, detail::poll_model(descriptor, poll_mask), submit_mode::queued);
}

inline auto io_context::async_poll_direct(async_io::descriptor_view descriptor,
                                          unsigned poll_mask) {
  return detail::native_io_sender(
      *this, detail::poll_model(descriptor, poll_mask), submit_mode::direct);
}

inline auto io_context::async_resolve(async_io::dns_query query,
                                      async_io::dns_result_view result) {
  return select_native_context().async_resolve(std::move(query), result);
}

inline auto io_context::async_resolve(std::string_view host,
                                      std::string_view service,
                                      async_io::dns_result_view result) {
  return async_resolve(async_io::dns_query(host, service), result);
}

inline auto steady_timer::async_wait() {
  return detail::timer_wait_sender(*this);
}

template <io_context::schedule_kind Kind>
std::error_code io_context::basic_scheduler<Kind>::flush_io_queue()
    const noexcept {
  return context_->flush_io_queue();
}

template <io_context::schedule_kind Kind>
std::size_t io_context::basic_scheduler<Kind>::queued_io_size() const noexcept {
  return context_->queued_io_size();
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read(
    async_io::stream_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_read(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read_some(
    async_io::stream_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_read_some(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read_direct(
    async_io::stream_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_read_direct(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read_some_direct(
    async_io::stream_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_read_some_direct(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write(
    async_io::stream_socket_view socket, const_buffer buffer, int flags) const {
  return context_->async_write(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write_some(
    async_io::stream_socket_view socket, const_buffer buffer, int flags) const {
  return context_->async_write_some(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write_direct(
    async_io::stream_socket_view socket, const_buffer buffer, int flags) const {
  return context_->async_write_direct(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write_some_direct(
    async_io::stream_socket_view socket, const_buffer buffer, int flags) const {
  return context_->async_write_some_direct(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_receive(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_receive(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_receive_direct(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_receive_direct(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_send(
    async_io::datagram_socket_view socket, const_buffer buffer,
    int flags) const {
  return context_->async_send(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_send_direct(
    async_io::datagram_socket_view socket, const_buffer buffer,
    int flags) const {
  return context_->async_send_direct(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_receive_from(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    ip::endpoint& endpoint, int flags) const {
  return context_->async_receive_from(socket, buffer, endpoint, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_receive_from_direct(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    ip::endpoint& endpoint, int flags) const {
  return context_->async_receive_from_direct(socket, buffer, endpoint, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_send_to(
    async_io::datagram_socket_view socket, const_buffer buffer,
    const ip::endpoint& endpoint, int flags) const {
  return context_->async_send_to(socket, buffer, endpoint, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_send_to_direct(
    async_io::datagram_socket_view socket, const_buffer buffer,
    const ip::endpoint& endpoint, int flags) const {
  return context_->async_send_to_direct(socket, buffer, endpoint, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read(
    async_io::descriptor_view descriptor, mutable_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_read(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read_some(
    async_io::descriptor_view descriptor, mutable_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_read_some(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read_direct(
    async_io::descriptor_view descriptor, mutable_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_read_direct(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read_some_direct(
    async_io::descriptor_view descriptor, mutable_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_read_some_direct(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write(
    async_io::descriptor_view descriptor, const_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_write(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write_some(
    async_io::descriptor_view descriptor, const_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_write_some(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write_direct(
    async_io::descriptor_view descriptor, const_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_write_direct(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write_some_direct(
    async_io::descriptor_view descriptor, const_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_write_some_direct(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_accept(
    async_io::stream_socket_view socket, int flags) const {
  return context_->async_accept(socket, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_accept_direct(
    async_io::stream_socket_view socket, int flags) const {
  return context_->async_accept_direct(socket, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_connect(
    async_io::stream_socket_view socket, const ip::endpoint& endpoint) const {
  return context_->async_connect(socket, endpoint);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_connect_direct(
    async_io::stream_socket_view socket, const ip::endpoint& endpoint) const {
  return context_->async_connect_direct(socket, endpoint);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_poll(
    async_io::descriptor_view descriptor, unsigned poll_mask) const {
  return context_->async_poll(descriptor, poll_mask);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_poll_direct(
    async_io::descriptor_view descriptor, unsigned poll_mask) const {
  return context_->async_poll_direct(descriptor, poll_mask);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_resolve(
    async_io::dns_query query, async_io::dns_result_view result) const {
  return context_->async_resolve(std::move(query), result);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_resolve(
    std::string_view host, std::string_view service,
    async_io::dns_result_view result) const {
  return context_->async_resolve(host, service, result);
}

}  // namespace bupp

#endif  // BUPP_LINUX_IO_CONTEXT_H_
