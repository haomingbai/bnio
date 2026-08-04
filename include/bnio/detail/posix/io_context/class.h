/**
 * @file class.h
 * @brief io_context class declaration.
 */

#pragma once
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#define BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_

#include <bnio/async_io/socket_view.h>
#include <bnio/async_io/time.h>
#include <bnio/buffer.h>
#include <bnio/detail/posix/io_context/native_context.h>
#include <bnio/detail/posix/io_context/options.h>
#include <bnio/detail/posix/io_context/state.h>
#include <bnio/detail/posix/io_context/steady_timer.h>
#include <bnio/detail/posix/io_context/timer_types.h>
#include <bnio/export.h>
#include <bnio/io_context_cpo.h>
#include <bnio/ip.h>

#include <atomic>
#include <bexec/completion_signatures.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <thread>

#include "bnio/async_io/dns/query.h"

namespace bnio {

enum class ssl_handshake_type;

namespace detail {
class descriptor_write_all_state;
class socket_write_all_state;
template <class Request, class Receiver>
class native_io_operation;
template <class Receiver>
class native_poll_operation;
template <class Receiver>
class resolve_operation;
}  // namespace detail

/**
 * High-level asynchronous I/O context for the configured native backend.
 *
 * io_context adapts the non-owning async_io views into sender-returning
 * operations. Higher-level stream owners build on top of these view-level
 * operations instead of being known by io_context.
 */
class BNIO_EXPORT io_context {
 public:
  /** I/O operation passively consumed by one native run-loop worker. */
  using operation_base = detail::native_io_operation_base;

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
        bexec::completion_signatures<bexec::set_value_t(std::error_code),
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
    class operation : public detail::native_operation_base {
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

        // The submission critical section (locked state check + enqueue)
        // is ordered against stop()'s state transition by the submit lock.
        // If the context is already stopping, publish_cpu() rejects the
        // enqueue and we complete inline so the operation never strands.
        if (!context_->publish_cpu(*this)) {
          complete();
        }
      }

      /**
       * Delivers the schedule completion.
       */
      void execute() noexcept override { complete(); }

     private:
      void complete() noexcept {
        // io_context::stop() is the sole source of set_stopped().
        if (context_->is_stopped()) {
          bexec::set_stopped(std::move(receiver_));
          return;
        }
        auto env = bexec::get_env(receiver_);
        auto token = bexec::query(env, bexec::get_stop_token);
        if (token.stop_requested()) {
          bexec::set_value(std::move(receiver_),
                           std::make_error_code(std::errc::operation_canceled));
        } else {
          bexec::set_value(std::move(receiver_), std::error_code{});
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
   * Sender returned by io_context::join().
   */
  class join_sender {
   public:
    using completion_signatures =
        bexec::completion_signatures<bexec::set_value_t()>;

    explicit join_sender(io_context& context) noexcept
        : context_(&context) {}

    template <class Receiver>
    class operation {
     public:
      operation(io_context& context, Receiver receiver)
          : context_(&context), receiver_(std::move(receiver)) {}

      operation(const operation&) = delete;
      operation& operator=(const operation&) = delete;
      operation(operation&&) = delete;
      operation& operator=(operation&&) = delete;

      void start() noexcept {
        if (context_->begin_stop()) {
          // This thread is responsible for stopping.
          context_->stop_internal();
          context_->stopped_.store(true, std::memory_order_release);
          bexec::set_value(std::move(receiver_));
        } else {
          // Another thread is already stopping or the context is stopped.
          while (!context_->stopped_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
          bexec::set_value(std::move(receiver_));
        }
      }

     private:
      io_context* context_;
      Receiver receiver_;
    };

    template <class Receiver>
    [[nodiscard]] auto connect(Receiver receiver) const {
      return operation<std::remove_cvref_t<Receiver>>(*context_,
                                                      std::move(receiver));
    }

   private:
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
   * Copy construction is disabled because the context owns a native context
   * and synchronization resources.
   */
  io_context(const io_context&) = delete;

  /**
   * Copy assignment is disabled because the context owns a native context
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
   * Returns whether the native backend was available when this context was
   * created.
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
   * Returns a sender that stops the context (if not already done) and waits
   * for it to fully finish.  If multiple threads call join(), exactly one
   * performs the actual stop and the rest spin-wait.
   *
   * The sender completes with set_value() when all workers have exited.
   */
  [[nodiscard]] join_sender join() noexcept;

  /**
   * Returns whether the current thread is running this context.
   */
  [[nodiscard]] bool is_in_context() const noexcept;

  /**
   * Returns whether stop() has been requested for this context.
   *
   * schedule_sender uses this to decide between set_stopped() (sole exit
   * for set_stopped) and set_value(ec) / set_value({}).
   */
  [[nodiscard]] bool is_stopped() const noexcept {
    return global_state_.life_state.load(std::memory_order_acquire) != 0 ||
           stopped_.load(std::memory_order_acquire);
  }

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
  friend class detail::descriptor_write_all_state;
  friend class detail::socket_write_all_state;
  template <class Receiver>
  friend class detail::timer_wait_operation;
  template <class Request, class Receiver>
  friend class detail::native_io_operation;
  template <class Receiver>
  friend class detail::native_poll_operation;
  template <class Receiver>
  friend class detail::resolve_operation;
  friend class join_sender;

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
   * Publishes an operation for passive native submission by a worker.
   *
   * Mirrors publish_cpu: checks the shutdown state under the submit lock
   * and, only while the context is not stopping, enqueues the operation
   * and wakes a sleeping worker as needed. The wake-channel write is bound
   * to the submit lock (see wake_locked), so it can never race the
   * destructor's close.
   *
   * @return true if the operation was published; false if the context is
   *         already stopping and the operation was NOT enqueued — the
   *         caller must complete it inline (set_stopped), mirroring the
   *         abort path.
   */
  [[nodiscard]] bool publish_io(operation_base& operation) noexcept;

  /**
   * Publishes CPU work for execution by one context run-loop worker.
   *
   * Checks the shutdown state under the submit lock and, only while the
   * context is not stopping, enqueues the operation and wakes a sleeping
   * worker as needed. publish_cpu assumes the publish happens against a
   * running context; the in-lock state check makes that assumption explicit
   * and atomic with the enqueue.
   *
   * @return true if the operation was published (enqueued, or posted to the
   *         worker-local queue); false if the context is already stopping
   *         and the operation was NOT enqueued — the caller must complete
   *         it inline (e.g. set_stopped) so it never strands.
   *
   * Only state-involving work (check, enqueue, wake decision) runs inside
   * the critical section; the caller executes the operation afterwards.
   */
  [[nodiscard]] bool publish_cpu(detail::native_operation_base& operation) noexcept;

  void wake_one_worker() noexcept;

  /** Wakes one worker only when every published worker is sleeping. */
  void wake_one_if_all_workers_sleeping() noexcept;

  // Internal wake helpers. The *_locked variants require the caller to
  // already hold global_state_.submit_lock; they are used by the publish
  // paths so the wake-channel write stays bound to the submit lock without
  // re-locking. The public wake_one_worker / wake_one_if_all_workers_sleeping
  // acquire the lock themselves and re-check the shutdown state first:
  // after ~io_context has published the terminal state and closed the
  // channel under the same lock, a wake would write to a closed fd.
  void wake_locked() noexcept;
  void wake_if_all_sleeping_locked() noexcept;

  void register_timer(detail::timer_slot& timer) noexcept;

  void unregister_timer(detail::timer_slot& timer) noexcept;

  [[nodiscard]] std::size_t cancel_timer(detail::timer_slot& timer) noexcept;

  [[nodiscard]] std::size_t set_timer_expiry(detail::timer_slot& timer,
                                             time_point expiry) noexcept;

  [[nodiscard]] time_point timer_expiry(
      const detail::timer_slot& timer) const noexcept;

  void start_timer_wait(detail::timer_operation_base& operation,
                        detail::timer_slot& timer) noexcept;

  [[nodiscard]] bool try_fetch_timeout_operations(
      time_point& deadline,
      detail::native_operation_base*& operations) noexcept;
  [[nodiscard]] static bool try_fetch_timeout_operations_thunk(
      void* state, time_point& deadline,
      detail::native_operation_base*& operations) noexcept;

  /** Aborts all pending timer waits with set_stopped (io_context::stop only).
   *
   *  Drains every active and inactive timer slot's submitted queue, sets
   *  each operation's completion to timer_completion_kind::stopped, and
   *  pushes them to timers_.ready.  Worker finish() picks them up via
   *  consume_timeout_operations().
   */
  void abort_pending_timer_waits() noexcept;

  /**
   * Performs the actual stop work (abort timers, wake and wait for
   * workers).  Assumes the caller already owns the stopping CAS,
   * i.e. life_state == stopping.
   */
  int stop_internal() noexcept;

  /**
   * Elects this thread as the stopping thread and publishes the stopping
   * state inside the submit-path lock.
   *
   * Ordering the state transition against publish_cpu()'s
   * check-state + enqueue critical section (same lock) is what binds
   * enqueue to state: operations that passed the check are drained before
   * workers exit, and operations that see the stopping state do not
   * enqueue. The election CAS on stop_requested_ keeps exactly one
   * stopping thread while stop() and join() share this path.
   *
   * @return true if this thread owns the stop, false if another thread
   *         already owns it (the caller must wait for stopped_).
   */
  [[nodiscard]] bool begin_stop() noexcept;

  // Consumes the timer's lock-protected list of active wait operations.
  [[nodiscard]] detail::timer_operation_queue take_timer_operations_locked(
      detail::timer_slot& timer) noexcept;

  void enqueue_timer_operations_locked(
      detail::timer_operation_base* operations,
      detail::timer_completion_kind completion) noexcept;

  void queue_timer_completion(
      detail::timer_operation_base& operation,
      detail::timer_completion_kind completion) noexcept;

  detail::native_task_queue_state global_state_;
  detail::native_context_state native_;

  /** Worker lifecycle counter. Incremented at the top of run(), decremented
   *  on every return path. io_context::stop() polls this until zero. */
  std::atomic<std::size_t> running_workers_{0};

  /** Stop-thread election flag. Exactly one thread wins the CAS and then
   *  publishes life_state inside the submit-path lock (see begin_stop()). */
  std::atomic<int> stop_requested_{0};

  /** Signalled when the context has fully stopped. Join() waiters spin
   *  on this flag. Independent from life_state to avoid torn reads. */
  std::atomic_bool stopped_{false};

  detail::timer_state_data timers_;
  std::atomic_bool native_available_{false};

  static thread_local io_context* current_context_;
  static thread_local detail::native_context* current_worker_native_;
};

}  // namespace bnio

#include <bnio/detail/posix/io_context/native_io.h>

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
