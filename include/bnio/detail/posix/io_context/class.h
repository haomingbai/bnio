/**
 * @file class.h
 * @brief io_context class declaration.
 */

#pragma once
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#define BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_

#include <bnio/async_io/local/socket_view.h>
#include <bnio/async_io/random_access_file.h>
#include <bnio/async_io/socket_view.h>
#include <bnio/async_io/stop_transition.h>
#include <bnio/async_io/time.h>
#include <bnio/buffer.h>
#include <bnio/detail/error_code.h>
#include <bnio/detail/posix/io_context/native_context.h>
#include <bnio/detail/posix/io_context/options.h>
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

#include "bnio/async_io/dns/query.h"

namespace bnio {

namespace detail {

/**
 * Scheduling policy used by io_context scheduler handles.
 *
 * Lives at bnio::detail scope; io_context re-exposes it as
 * io_context::schedule_kind.
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

  /**
   * Publish schedule() and scheduler-initiated I/O through the shared
   * queue, skipping the worker-local fast path; any worker may drain it.
   */
  defer,
};

template <schedule_kind Kind>
class stream_file_write_all_state;
template <schedule_kind Kind>
class random_access_write_all_state;
template <schedule_kind Kind>
class socket_write_all_state;
template <schedule_kind Kind, class Request, class Control, class Receiver>
class native_io_operation;
template <schedule_kind Kind, class Receiver>
class native_poll_operation;
template <schedule_kind Kind, class Receiver>
class resolve_operation;
template <schedule_kind Kind>
struct schedule_policy;
}  // namespace detail

/**
 * Asynchronous I/O context and scheduler factory. run() drives the native
 * loop, one native context per calling thread; schedulers return senders over
 * non-owning async_io views. Non-movable, and must outlive every submitted
 * operation.
 */
class BNIO_EXPORT io_context {
 public:
  /** I/O operation passively consumed by one native run-loop worker. */
  using operation_base = detail::native_io_operation_base;

  /** Monotonic clock used by this context. */
  using steady_clock = async_io::steady_clock;

  /** Default clock used by this context. */
  using clock = async_io::clock;

  /** Default duration type used by this context. */
  using duration = async_io::duration;

  /** Default time point type used by this context. */
  using time_point = async_io::time_point;

  /** Scheduling policy; alias of bnio::detail::schedule_kind. */
  using schedule_kind = detail::schedule_kind;

  /** Sender returned by a scheduler's schedule() member. */
  template <schedule_kind Kind>
  class schedule_sender {
   public:
    /** Completion signatures produced by the scheduler sender. */
    using completion_signatures =
        bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                     bexec::set_stopped_t()>;

    /** Creates a schedule sender bound to the context. */
    explicit schedule_sender(io_context& context) noexcept
        : context_(&context) {}

    /** Operation state for a scheduler schedule() sender. */
    template <class Receiver>
    class operation : public detail::native_operation_base {
     public:
      /** Creates an operation bound to the context and receiver. */
      operation(io_context& context, Receiver receiver)
          : context_(&context), receiver_(std::move(receiver)) {}

      /** Deleted: operations are queued intrusively. */
      operation(const operation&) = delete;

      /** Deleted: operations are queued intrusively. */
      operation& operator=(const operation&) = delete;

      /** Deleted: operations are queued intrusively. */
      operation(operation&&) = delete;

      /** Deleted: operations are queued intrusively. */
      operation& operator=(operation&&) = delete;

      /** Starts the schedule operation. */
      void start() noexcept {
        if constexpr (Kind == schedule_kind::dispatch) {
          if (context_->is_in_context()) {
            complete();
            return;
          }
        }

        // If the context is already stopping the publish is rejected;
        // complete inline so the operation never strands.
        if (!detail::schedule_policy<Kind>::publish_cpu(*context_, *this)) {
          complete();
        }
      }

      /** Delivers the schedule completion. */
      void execute() noexcept override { complete(); }

     private:
      void complete() noexcept {
        // A cancelled receiver stop token wins with set_stopped(); otherwise
        // a stopping context yields set_value(operation_canceled).
        auto env = bexec::get_env(receiver_);
        auto token = bexec::query(env, bexec::get_stop_token);
        if (token.stop_requested()) {
          bexec::set_stopped(std::move(receiver_));
          return;
        }
        if (context_->is_stopped()) {
          bexec::set_value(std::move(receiver_),
                           std::make_error_code(std::errc::operation_canceled));
          return;
        }
        bexec::set_value(std::move(receiver_), bnio::detail::empty_error_code);
      }

      io_context* context_;
      Receiver receiver_;
    };

    template <class Receiver>
    /** Connects the schedule sender to @p receiver. */
    [[nodiscard]] auto connect(Receiver receiver) const {
      return operation<std::remove_cvref_t<Receiver>>(*context_,
                                                      std::move(receiver));
    }

   private:
    io_context* context_;
  };

  /** Copyable scheduler handle produced by io_context. */
  template <schedule_kind Kind>
  class basic_scheduler {
   public:
    /** Concrete sender type returned by schedule(). */
    using schedule_sender_type = schedule_sender<Kind>;

    /** Copies a scheduler handle. */
    basic_scheduler(const basic_scheduler&) noexcept = default;

    /** Assigns a scheduler handle. */
    basic_scheduler& operator=(const basic_scheduler&) noexcept = default;

    /** Returns a sender that completes per this scheduler's policy. */
    [[nodiscard]] schedule_sender_type schedule() const noexcept {
      return schedule_sender_type(*context_);
    }

    /** Returns the context that owns this scheduler. */
    [[nodiscard]] io_context& context() const noexcept { return *context_; }

    /** Reads the whole buffer from a socket. */
    [[nodiscard]] auto async_read(async_io::stream_socket_view socket,
                                  mutable_buffer buffer, int flags = 0) const;

    /** Performs one socket read operation. */
    [[nodiscard]] auto async_read_some(async_io::stream_socket_view socket,
                                       mutable_buffer buffer,
                                       int flags = 0) const;

    /** Writes the whole buffer to a socket. */
    [[nodiscard]] auto async_write(async_io::stream_socket_view socket,
                                   const_buffer buffer, int flags = 0) const;

    /** Performs one socket write; short writes are not retried. */
    [[nodiscard]] auto async_write_some(async_io::stream_socket_view socket,
                                        const_buffer buffer,
                                        int flags = 0) const;

    /** Reads the whole buffer from a local stream socket. */
    [[nodiscard]] auto async_read(async_io::local::stream_socket_view socket,
                                  mutable_buffer buffer, int flags = 0) const;

    /** Performs one local stream socket read. */
    [[nodiscard]] auto async_read_some(
        async_io::local::stream_socket_view socket, mutable_buffer buffer,
        int flags = 0) const;

    /** Writes the whole buffer to a local stream socket. */
    [[nodiscard]] auto async_write(async_io::local::stream_socket_view socket,
                                   const_buffer buffer, int flags = 0) const;

    /** Performs one local socket write; short writes are not retried. */
    [[nodiscard]] auto async_write_some(
        async_io::local::stream_socket_view socket, const_buffer buffer,
        int flags = 0) const;

    /** Receives one datagram on a connected socket. */
    [[nodiscard]] auto async_receive(async_io::datagram_socket_view socket,
                                     mutable_buffer buffer,
                                     int flags = 0) const;

    /** Sends one datagram on a connected socket. */
    [[nodiscard]] auto async_send(async_io::datagram_socket_view socket,
                                  const_buffer buffer, int flags = 0) const;

    /** Receives one datagram and stores the source in @p endpoint. */
    [[nodiscard]] auto async_receive_from(async_io::datagram_socket_view socket,
                                          mutable_buffer buffer,
                                          ip::endpoint& endpoint,
                                          int flags = 0) const;

    /** Sends one datagram to @p endpoint; no connect() required. */
    [[nodiscard]] auto async_send_to(async_io::datagram_socket_view socket,
                                     const_buffer buffer,
                                     const ip::endpoint& endpoint,
                                     int flags = 0) const;

    /** Receives one local datagram on a connected socket. */
    [[nodiscard]] auto async_receive(async_io::local::datagram_socket_view,
                                     mutable_buffer buffer,
                                     int flags = 0) const;

    /** Sends one local datagram on a connected socket. */
    [[nodiscard]] auto async_send(async_io::local::datagram_socket_view socket,
                                  const_buffer buffer, int flags = 0) const;

    /** Receives one local datagram and stores the source in @p endpoint. */
    [[nodiscard]] auto async_receive_from(
        async_io::local::datagram_socket_view socket, mutable_buffer buffer,
        async_io::local::endpoint& endpoint, int flags = 0) const;

    /** Sends one local datagram to @p endpoint; no connect() required. */
    [[nodiscard]] auto async_send_to(async_io::local::datagram_socket_view,
                                     const_buffer buffer,
                                     const async_io::local::endpoint& endpoint,
                                     int flags = 0) const;
    /** Reads the whole buffer from a descriptor, advancing its position. */
    [[nodiscard]] auto async_read(async_io::descriptor_view descriptor,
                                  mutable_buffer buffer) const;

    /** Performs one streaming descriptor read. */
    [[nodiscard]] auto async_read_some(async_io::descriptor_view descriptor,
                                       mutable_buffer buffer) const;

    /** Writes the whole buffer to a descriptor, advancing its position. */
    [[nodiscard]] auto async_write(async_io::descriptor_view descriptor,
                                   const_buffer buffer) const;

    /** Performs one descriptor write; short writes are not retried. */
    [[nodiscard]] auto async_write_some(async_io::descriptor_view descriptor,
                                        const_buffer buffer) const;

    /** Reads the whole buffer from a random access file at @p offset. */
    [[nodiscard]] auto async_read(async_io::random_access_file file,
                                  mutable_buffer buffer,
                                  std::uint64_t offset) const;

    /** Performs one random access read at @p offset. */
    [[nodiscard]] auto async_read_some(async_io::random_access_file file,
                                       mutable_buffer buffer,
                                       std::uint64_t offset) const;

    /** Writes the whole buffer to a random access file at @p offset. */
    [[nodiscard]] auto async_write(async_io::random_access_file file,
                                   const_buffer buffer,
                                   std::uint64_t offset) const;

    /** Performs one random access write, no short-write retry. */
    [[nodiscard]] auto async_write_some(async_io::random_access_file file,
                                        const_buffer buffer,
                                        std::uint64_t offset) const;

    /** Accepts one connection. */
    [[nodiscard]] auto async_accept(async_io::stream_socket_view socket,
                                    int flags = 0) const;

    /** Connects a socket to an endpoint. */
    [[nodiscard]] auto async_connect(async_io::stream_socket_view socket,
                                     const ip::endpoint& endpoint) const;

    /** Accepts one connection on a local stream socket. */
    [[nodiscard]] auto async_accept(async_io::local::stream_socket_view socket,
                                    int flags = 0) const;

    /** Connects a local stream socket to a local endpoint. */
    [[nodiscard]] auto async_connect(
        async_io::local::stream_socket_view socket,
        const async_io::local::endpoint& endpoint) const;

    /** Waits for events on a descriptor. */
    [[nodiscard]] auto async_poll(async_io::descriptor_view descriptor,
                                  unsigned poll_mask) const;

    /** Resolves a DNS query into caller-provided storage. */
    [[nodiscard]] auto async_resolve(async_io::dns_query query,
                                     async_io::dns_result_view result) const;

    /** Resolves a host and service into caller-provided storage. */
    [[nodiscard]] auto async_resolve(std::string_view host,
                                     std::string_view service,
                                     async_io::dns_result_view result) const;

    /** Whether two handles refer to the same context. */
    friend bool operator==(basic_scheduler lhs, basic_scheduler rhs) noexcept {
      return lhs.context_ == rhs.context_;
    }

   private:
    friend class io_context;

    explicit basic_scheduler(io_context& context) noexcept
        : context_(&context) {}

    io_context* context_;
  };

  /** Sender returned by io_context::join(). */
  class join_sender {
   public:
    /** Completes with set_value() when the context has fully stopped. */
    using completion_signatures =
        bexec::completion_signatures<bexec::set_value_t()>;

    /** Creates a join sender bound to @p context. */
    explicit join_sender(io_context& context) noexcept : context_(&context) {}

    /** Operation state that stops the context and waits for it to finish. */
    template <class Receiver>
    class operation {
     public:
      /** Creates an operation bound to the context and receiver. */
      operation(io_context& context, Receiver receiver)
          : context_(&context), receiver_(std::move(receiver)) {}

      operation(const operation&) = delete;
      operation& operator=(const operation&) = delete;
      operation(operation&&) = delete;
      operation& operator=(operation&&) = delete;

      /**
       * Drives Phase 2 if needed, then waits for every worker and drains
       * shared work. Must not run on a context worker or a stop/join drain
       * receiver.
       */
      void start() noexcept {
        context_->join_internal();
        bexec::set_value(std::move(receiver_));
      }

     private:
      io_context* context_;
      Receiver receiver_;
    };

    /** Connects the join sender to @p receiver. */
    template <class Receiver>
    [[nodiscard]] auto connect(Receiver receiver) const {
      return operation<std::remove_cvref_t<Receiver>>(*context_,
                                                      std::move(receiver));
    }

   private:
    io_context* context_;
  };

  /** Scheduler with Asio dispatch-like schedule() semantics. */
  using dispatch_scheduler = basic_scheduler<schedule_kind::dispatch>;

  /** Scheduler with Asio post-like schedule() semantics. */
  using post_scheduler = basic_scheduler<schedule_kind::post>;

  /** Scheduler whose schedule() and I/O always use the shared queue. */
  using defer_scheduler = basic_scheduler<schedule_kind::defer>;

  /** Creates a context with default options. */
  io_context() noexcept;

  /** Creates a context with explicit options. */
  explicit io_context(const io_context_options& options) noexcept;

  /** Releases worker, timer, and native context resources. */
  ~io_context() noexcept;

  /** Deleted: the context owns native and synchronization resources. */
  io_context(const io_context&) = delete;

  /** Deleted: the context owns native and synchronization resources. */
  io_context& operator=(const io_context&) = delete;

  /** Deleted: the context owns synchronization and timer state. */
  io_context(io_context&&) = delete;

  /** Deleted: the context owns synchronization and timer state. */
  io_context& operator=(io_context&&) = delete;

  /**
   * Always true: the POSIX backend is selected at compile time. Native
   * backend failure is reported by run() instead.
   */
  [[nodiscard]] bool is_open() const noexcept;

  /**
   * Runs the event loop on the calling thread until the context stops;
   * each call creates one native context for this thread.
   *
   * @return Empty for a normal run, including one that ends because of a
   *         stop requested after entry; operation_canceled when entry is
   *         rejected after a stop request (published work is drained first);
   *         otherwise the native entry error.
   */
  std::error_code run() noexcept;

  /**
   * Phase 1 of the two-phase stop: records the stop request and broadcasts
   * the wake channel. Takes no lock (not timers_.mutex, not submit_lock),
   * aborts no timer, publishes no stopping state, drains nothing, and waits
   * for nothing. Idempotent, and the only context operation valid from an
   * async signal handler.
   *
   * The abort/publish/drain half (phase 2) is driven by join(), stop()'s
   * no-worker fallback, run()'s rejected entry, or an exiting worker.
   *
   * The context must outlive any signal handler that calls this: the
   * destructor closes the shared wake channel, so a handler running
   * concurrently with destruction is unsupported.
   */
  void request_stop() noexcept;

  /**
   * Requests stop and returns immediately. With no worker running, this call
   * also completes Phase 2 (abort pending timers, publish stopping) and
   * delivers published work inline before returning; otherwise an exiting
   * worker or join() completes it. This is not a barrier: use join() to wait
   * for every run() to return. Inflight I/O and queued work complete through
   * the stop channel.
   *
   * Not async-signal-safe: use request_stop() from a signal handler.
   *
   * @return Always 0.
   */
  int stop() noexcept;

  /**
   * Stops if needed and waits for every run() to return, then drains the
   * shared queues. Concurrent callers wait for the same state and may drain
   * concurrently; every operation is still delivered exactly once.
   *
   * Must not run on a context worker or a stop/join drain receiver: it waits
   * for itself and never completes.
   */
  [[nodiscard]] join_sender join() noexcept;

  /** Whether the current thread is running this context. */
  [[nodiscard]] bool is_in_context() const noexcept;

  /**
   * Whether a stop has been requested. True immediately after request_stop();
   * schedule_sender arbitration consults this only after the receiver's stop
   * token.
   */
  [[nodiscard]] bool is_stopped() const noexcept {
    return global_state_.stop_requested.load(std::memory_order_acquire) != 0 ||
           global_state_.life_state.load(std::memory_order_acquire) !=
               static_cast<int>(async_io::stop_transition::none);
  }

  /**
   * Whether eager immediate I/O completion is enabled; fixed at
   * construction and read without synchronization afterwards.
   */
  [[nodiscard]] bool enable_immediate_io() const noexcept {
    return immutable_flags_.enable_immediate_io;
  }

  /** Scheduler whose schedule() may complete inline on the context thread. */
  [[nodiscard]] dispatch_scheduler get_dispatch_scheduler() noexcept;

  /** Scheduler whose schedule() always posts through the context loop. */
  [[nodiscard]] post_scheduler get_post_scheduler() noexcept;

  /**
   * Scheduler whose schedule() and scheduler-initiated I/O always use the
   * shared queue, never a worker's local queue.
   */
  [[nodiscard]] defer_scheduler get_defer_scheduler() noexcept;

 private:
  friend class steady_timer;
  friend class detail::timer_operation_base;
  template <schedule_kind Kind>
  friend class detail::stream_file_write_all_state;
  template <schedule_kind Kind>
  friend class detail::random_access_write_all_state;
  template <schedule_kind Kind>
  friend class detail::socket_write_all_state;
  template <class Receiver>
  friend class detail::timer_wait_operation;
  template <schedule_kind Kind, class Request, class Control, class Receiver>
  friend class detail::native_io_operation;
  template <schedule_kind Kind, class Receiver>
  friend class detail::native_poll_operation;
  template <schedule_kind Kind, class Receiver>
  friend class detail::resolve_operation;
  // The schedule policy composes the private publish primitives on behalf
  // of the operation layer.
  template <schedule_kind Kind>
  friend struct detail::schedule_policy;
  friend class join_sender;

  /**
   * Reads bytes from a non-owning stream socket view; completes with the
   * byte count.
   */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_read(async_io::stream_socket_view socket,
                                mutable_buffer buffer, int flags = 0);

  /** One socket read through a non-owning stream socket view. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_read_some(async_io::stream_socket_view socket,
                                     mutable_buffer buffer, int flags = 0);

  /** Writes the whole buffer through a non-owning stream socket view. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_write(async_io::stream_socket_view socket,
                                 const_buffer buffer, int flags = 0);

  /** One socket write through a non-owning stream socket view. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_write_some(async_io::stream_socket_view socket,
                                      const_buffer buffer, int flags = 0);

  /** Reads the whole buffer from a local stream socket view. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_read(async_io::local::stream_socket_view socket,
                                mutable_buffer buffer, int flags = 0);

  /** One local stream socket read. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_read_some(async_io::local::stream_socket_view,
                                     mutable_buffer buffer, int flags = 0);

  /** Writes the whole buffer to a local stream socket view. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_write(async_io::local::stream_socket_view socket,
                                 const_buffer buffer, int flags = 0);

  /** One local stream socket write. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_write_some(async_io::local::stream_socket_view,
                                      const_buffer buffer, int flags = 0);

  /** Reads bytes from a descriptor, advancing the file position. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_read(async_io::descriptor_view descriptor,
                                mutable_buffer buffer);

  /** One streaming descriptor read. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_read_some(async_io::descriptor_view descriptor,
                                     mutable_buffer buffer);

  /** Writes the whole buffer to a descriptor, advancing its position. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_write(async_io::descriptor_view descriptor,
                                 const_buffer buffer);

  /** One streaming descriptor write. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_write_some(async_io::descriptor_view descriptor,
                                      const_buffer buffer);

  /** Reads bytes from a random access file at an explicit offset. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_read(async_io::random_access_file file,
                                mutable_buffer buffer, std::uint64_t offset);

  /** One random access read at an explicit offset. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_read_some(async_io::random_access_file file,
                                     mutable_buffer buffer,
                                     std::uint64_t offset);

  /** Writes the whole buffer to a random access file at @p offset. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_write(async_io::random_access_file file,
                                 const_buffer buffer, std::uint64_t offset);

  /** One positioned write to a random access file. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_write_some(async_io::random_access_file file,
                                      const_buffer buffer,
                                      std::uint64_t offset);

  /** Receives one datagram on a connected socket. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_receive(async_io::datagram_socket_view socket,
                                   mutable_buffer buffer, int flags = 0);

  /** Sends one datagram on a connected socket. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_send(async_io::datagram_socket_view socket,
                                const_buffer buffer, int flags = 0);

  /** Receives one datagram and stores the source in @p endpoint. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_receive_from(async_io::datagram_socket_view socket,
                                        mutable_buffer buffer,
                                        ip::endpoint& endpoint, int flags = 0);

  /** Sends one datagram to @p endpoint; no connect() required. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_send_to(async_io::datagram_socket_view socket,
                                   const_buffer buffer,
                                   const ip::endpoint& endpoint, int flags = 0);

  /** Receives one local datagram on a connected socket. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_receive(async_io::local::datagram_socket_view,
                                   mutable_buffer buffer, int flags = 0);

  /** Sends one local datagram on a connected socket. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_send(async_io::local::datagram_socket_view socket,
                                const_buffer buffer, int flags = 0);

  /** Receives one local datagram and stores the source in @p endpoint. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_receive_from(
      async_io::local::datagram_socket_view socket, mutable_buffer buffer,
      async_io::local::endpoint& endpoint, int flags = 0);

  /** Sends one local datagram to @p endpoint; no connect() required. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_send_to(async_io::local::datagram_socket_view,
                                   const_buffer buffer,
                                   const async_io::local::endpoint& endpoint,
                                   int flags = 0);
  /** Accepts one connection from a non-owning listening socket view. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_accept(async_io::stream_socket_view socket,
                                  int flags = 0);

  /** Connects a non-owning stream socket view. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_connect(async_io::stream_socket_view socket,
                                   const ip::endpoint& endpoint);

  /** Accepts one connection on a listening local stream socket view. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_accept(async_io::local::stream_socket_view socket,
                                  int flags = 0);

  /** Connects a non-owning local stream socket view. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_connect(async_io::local::stream_socket_view socket,
                                   const async_io::local::endpoint& endpoint);

  /** Waits for events on a file descriptor. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_poll(async_io::descriptor_view descriptor,
                                unsigned poll_mask);

  /** Resolves a DNS query into caller storage on the context run loop. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_resolve(async_io::dns_query query,
                                   async_io::dns_result_view result);

  /** Resolves a host and service into caller storage on the run loop. */
  template <schedule_kind Kind = schedule_kind::post>
  [[nodiscard]] auto async_resolve(std::string_view host,
                                   std::string_view service,
                                   async_io::dns_result_view result);

  /**
   * Publishes an operation for passive submission by a worker. A worker of
   * this context posts to its own queue without the submit lock and always
   * succeeds; other threads enqueue under the lock only while Phase 2 has not
   * begun (life_state is none; request_stop() alone does not close it).
   *
   * @return false if stopping and not enqueued; the caller must then complete
   *         it inline with set_stopped, like the abort path.
   */
  [[nodiscard]] bool publish_io(operation_base& operation) noexcept;

  /**
   * Publishes CPU work for execution by a run-loop worker; same split and
   * false-means-complete-inline rule as publish_io().
   */
  [[nodiscard]] bool publish_cpu(
      detail::native_operation_base& operation) noexcept;

  /**
   * publish_io() shared path only, even for a running worker; false as
   * publish_io(), with the same complete-inline requirement.
   */
  [[nodiscard]] bool publish_io_deferred(operation_base& operation) noexcept;

  /**
   * publish_cpu() shared path only, as publish_io_deferred() relates to
   * publish_io(); false as publish_cpu().
   */
  [[nodiscard]] bool publish_cpu_deferred(
      detail::native_operation_base& operation) noexcept;

  /**
   * Native context this thread is running as a worker of, or nullptr when
   * this thread is not a worker of this io_context.
   *
   * Non-null only inside io_context::run(). The bound check also covers
   * nested run(): an outer worker's pointer stays bound, and posting to
   * that queue would strand the operation, so any other context falls
   * through to the shared queue. A non-null result means the caller itself
   * drains that local queue, which is what makes the unlocked, unchecked
   * local path safe even while stopping.
   */
  [[nodiscard]] detail::native_context* running_worker_native() const noexcept;

  /** Wakes one worker only when every published worker is sleeping. */
  void wake_one_if_all_workers_sleeping() noexcept;

  // The *_locked variants require global_state_.submit_lock, keeping the
  // wake-channel write bound to that lock so it cannot hit a closed fd.
  void wake_locked() noexcept;
  void wake_one_sleeping_locked() noexcept;

  /**
   * Whether a worker may enter the run loop: no stop requested and Phase 2
   * has not begun.
   */
  [[nodiscard]] bool can_start_run() const noexcept;

  /** Decrements the running worker count on every run() return path. */
  void release_worker_slot() noexcept;

  /** Builds, binds, and blocks in one worker's native run loop. */
  std::error_code run_native_loop() noexcept;

  /** Registers a timer slot by expiry in the active heap or inactive list. */
  void register_timer(detail::timer_slot& timer) noexcept;

  /**
   * Unregisters a timer slot. Its pending waits are detached and staged
   * with canceled completion, delivered as set_value(operation_canceled).
   */
  void unregister_timer(detail::timer_slot& timer) noexcept;

  /**
   * Detaches and cancels the timer's submitted waits, leaving its
   * active/inactive state and expiry unchanged.
   *
   * @return The number of canceled waits.
   */
  [[nodiscard]] std::size_t cancel_timer(detail::timer_slot& timer) noexcept;

  /**
   * Replaces the timer's expiry, detaching its submitted waits as canceled
   * and reinserting it by the new time.
   *
   * @return The number of detached waits.
   */
  [[nodiscard]] std::size_t set_timer_expiry(detail::timer_slot& timer,
                                             time_point expiry) noexcept;

  /** The timer's stored expiry; authoritative even if unregistered. */
  [[nodiscard]] time_point timer_expiry(
      const detail::timer_slot& timer) const noexcept;

  /**
   * Starts a wait: links it at an active timer's submitted head, or stages
   * an immediate completion for an already-expired timer without touching
   * the shared CPU queue.
   */
  void start_timer_wait(detail::timer_operation_base& operation,
                        detail::timer_slot& timer) noexcept;

  // The *_locked variants require timers_.mutex. Each returns whether a
  // sleeping worker must be woken; count-returning ones combine both.
  [[nodiscard]] bool register_timer_locked(detail::timer_slot& timer,
                                           time_point now) noexcept;

  [[nodiscard]] bool unregister_timer_locked(
      detail::timer_slot& timer) noexcept;

  [[nodiscard]] std::pair<std::size_t, bool> cancel_timer_locked(
      detail::timer_slot& timer) noexcept;

  [[nodiscard]] std::pair<std::size_t, bool> set_timer_expiry_locked(
      detail::timer_slot& timer, time_point expiry, time_point now) noexcept;

  [[nodiscard]] bool start_timer_wait_locked(
      detail::timer_operation_base& operation,
      detail::timer_slot& timer) noexcept;

  [[nodiscard]] bool try_fetch_timeout_operations(
      time_point& deadline,
      detail::native_operation_base*& operations) noexcept;
  [[nodiscard]] static bool try_fetch_timeout_operations_thunk(
      void* state, time_point& deadline,
      detail::native_operation_base*& operations) noexcept;

  // drain_expired_timers_locked runs under timers_.mutex;
  // reverse_ready_operations runs after it is released.
  void drain_expired_timers_locked(time_point now) noexcept;

  void reverse_ready_operations(
      detail::timer_operation_base* ready,
      detail::native_operation_base*& operations) noexcept;

  /**
   * Aborts pending timer waits into timers_.ready as canceled; they complete
   * with set_value(operation_canceled), not set_stopped(). Runs before
   * life_state is published, so workers entering finish() see the full list.
   */
  void abort_pending_timer_waits() noexcept;

  /** Drives Phase 2, waits for every worker, then drains shared work no worker
   *  consumed. */
  void join_internal() noexcept;

  /**
   * Completes Phase 2 if needed, then drains shared queues. Concurrent drains
   * are safe: queue pops are exclusive, so each operation is delivered
   * exactly once.
   */
  void drain_shared_queues_for_stop() noexcept;

  /**
   * Phase 2 of the two-phase stop, idempotent. The life_state CAS winner
   * aborts pending timer waits, publishes stopping under submit_lock, and
   * re-broadcasts; a losing caller waits until stopping is published.
   *
   * Only valid on a worker, join, or stop caller: never a signal handler.
   */
  void complete_stop() noexcept;

  /** Adapts complete_stop() to the shared task-queue hook signature. */
  static void complete_stop_thunk(void* state) noexcept;

  /** Consumes the timer's lock-protected list of submitted waits. */
  [[nodiscard]] detail::timer_operation_queue take_timer_operations_locked(
      detail::timer_slot& timer) noexcept;

  /**
   * Stages detached waits on the timer-ready list with @p completion; a
   * worker's passive timer check or a stop/join drain consumes it.
   */
  void enqueue_timer_operations_locked(
      detail::timer_operation_base* operations,
      detail::timer_completion_kind completion) noexcept;

  /** Stages one wait's completion kind on the timer-ready list. */
  void queue_timer_completion(
      detail::timer_operation_base& operation,
      detail::timer_completion_kind completion) noexcept;

  detail::native_task_queue_state global_state_;
  platform_io_context_options native_options_;

  /**
   * Configuration fixed at construction and read-only afterwards.
   *
   * enable_immediate_io is written once before the context is shared and
   * read as a plain bool on the hot path.
   */
  struct immutable_flags {
    explicit immutable_flags(bool eager_immediate_io) noexcept
        : enable_immediate_io(eager_immediate_io) {}

    /** Eager immediate I/O switch, fixed at construction (see options.h). */
    bool enable_immediate_io = true;
  } immutable_flags_;

  detail::timer_state_data timers_;

  static thread_local io_context* current_context_;
  static thread_local detail::native_context* current_worker_native_;
};

}  // namespace bnio

#include <bnio/detail/posix/io_context/native_io.h>
#include <bnio/detail/posix/io_context/schedule_policy.h>

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
