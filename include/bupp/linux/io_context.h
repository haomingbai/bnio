#pragma once
#ifndef BUPP_LINUX_IO_CONTEXT_H_
#define BUPP_LINUX_IO_CONTEXT_H_

#include <bupp/async_io/linux/io_uring_context.h>
#include <bupp/async_io/socket_view.h>
#include <bupp/async_io/time.h>
#include <bupp/export.h>
#include <bupp/ip.h>

#include <atomic>
#include <bexec/detail/manual_lifetime.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace bupp {

enum class ssl_handshake_type;

class io_context;
class steady_timer;

template <class NextLayer>
class ssl_stream;

namespace base {
class submission_queue_entry;
}  // namespace base

/** @cond BUPP_DETAIL */
namespace detail {

class timer_operation_base;
template <class Receiver>
class timer_wait_operation;
class timer_wait_sender;

enum class timer_completion_kind {
  value,
  stopped,
};

struct timer_heap_item {
  async_io::time_point deadline{};
  std::uint64_t timer_id = 0;
  std::uint64_t generation = 0;
};

class timer_operation_base
    : public async_io::linux_native::io_uring_operation_base {
 public:
  explicit timer_operation_base(io_context& context) noexcept;

  timer_operation_base(const timer_operation_base&) = delete;
  timer_operation_base& operator=(const timer_operation_base&) = delete;
  timer_operation_base(timer_operation_base&&) = delete;
  timer_operation_base& operator=(timer_operation_base&&) = delete;

  ~timer_operation_base() override = default;

 protected:
  [[nodiscard]] timer_completion_kind timer_completion() const noexcept {
    return timer_completion_;
  }

  friend class bupp::io_context;

  io_context* timer_context_;
  timer_operation_base* timer_next_ = nullptr;
  timer_completion_kind timer_completion_ = timer_completion_kind::value;
};

struct timer_slot {
  mutable std::mutex mutex;
  io_context* context = nullptr;
  std::uint64_t id = 0;
  async_io::time_point expiry{};
  std::uint64_t generation = 0;
  std::atomic<timer_operation_base*> submitted_head{nullptr};
  timer_operation_base* waiting_head = nullptr;
};

}  // namespace detail
/** @endcond */

/**
 * Linux-specific options for the high-level io_context.
 */
struct linux_io_context_options {
  /**
   * Options passed to the underlying io_uring context.
   */
  async_io::linux_native::io_uring_context_options uring{};

  /**
   * Maximum queued operations before the queue is flushed immediately.
   */
  std::size_t max_queued_io_operations = 64;

  /**
   * Maximum time a queued operation waits before an automatic flush.
   */
  async_io::duration queued_io_flush_after = std::chrono::milliseconds(1);
};

/**
 * Platform-specific io_context options for the current build target.
 */
using platform_io_context_options = linux_io_context_options;

/**
 * Options used to construct a high-level io_context.
 */
struct io_context_options {
  /**
   * Requested concurrency hint for future platform implementations.
   */
  std::uint32_t concurrency_hint = 1;

  /**
   * Platform-specific options.
   */
  platform_io_context_options platform{};
};

/**
 * Submission policy for an asynchronous I/O operation.
 */
enum class submit_mode {
  /**
   * Queue the operation and submit it during a later flush.
   */
  queued,

  /**
   * Submit the operation immediately.
   */
  direct,
};

/**
 * io_context-bound steady-clock timer.
 */
class BUPP_EXPORT steady_timer {
 public:
  using clock = async_io::clock;
  using duration = async_io::duration;
  using time_point = async_io::time_point;

  explicit steady_timer(io_context& context) noexcept;

  steady_timer(io_context& context, time_point expiry) noexcept;

  template <class Rep, class Period>
  steady_timer(io_context& context,
               std::chrono::duration<Rep, Period> expiry_after) noexcept
      : steady_timer(context,
                     clock::now() +
                         std::chrono::duration_cast<duration>(expiry_after)) {}

  ~steady_timer() noexcept;

  steady_timer(const steady_timer&) = delete;
  steady_timer& operator=(const steady_timer&) = delete;
  steady_timer(steady_timer&& other) noexcept;
  steady_timer& operator=(steady_timer&& other) noexcept;

  [[nodiscard]] io_context& context() noexcept { return *timer_.context; }

  [[nodiscard]] const io_context& context() const noexcept {
    return *timer_.context;
  }

  [[nodiscard]] time_point expiry() const noexcept;

  [[nodiscard]] std::size_t expires_at(time_point expiry) noexcept;

  template <class Rep, class Period>
  [[nodiscard]] std::size_t expires_after(
      std::chrono::duration<Rep, Period> expiry_after) noexcept {
    return expires_at(clock::now() +
                      std::chrono::duration_cast<duration>(expiry_after));
  }

  [[nodiscard]] std::size_t cancel() noexcept;

  [[nodiscard]] auto async_wait();

 private:
  friend class io_context;
  friend class detail::timer_operation_base;
  template <class Receiver>
  friend class detail::timer_wait_operation;

  detail::timer_slot timer_;
};

/**
 * High-level asynchronous I/O context for Linux.
 *
 * io_context adapts the non-owning async_io views into sender-returning
 * operations. RAII owners such as tcp_socket and ssl_stream are supported only
 * as convenience overloads at this higher layer; the async_io vocabulary layer
 * remains non-owning.
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
     * Copy construction is disabled because operations are queued intrusively.
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
     * Intrusive next pointer used by the queued submission list.
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
   * Submits all operations currently waiting in the queued I/O list.
   */
  [[nodiscard]] std::error_code flush_io_queue() noexcept;

  /**
   * Returns the number of operations waiting in the queued I/O list.
   */
  [[nodiscard]] std::size_t queued_io_size() const noexcept;

  /**
   * Returns the underlying Linux native async I/O context.
   */
  [[nodiscard]] async_io::linux_native::io_uring_context&
  native_context() noexcept {
    return native_context_;
  }

  /**
   * Returns the underlying Linux native async I/O context.
   */
  [[nodiscard]] const async_io::linux_native::io_uring_context& native_context()
      const noexcept {
    return native_context_;
  }

  /**
   * Creates a queued sender that receives bytes from a non-owning stream socket
   * view.
   */
  template <class Buffer>
  [[nodiscard]] auto async_receive(async_io::stream_socket_view socket,
                                   Buffer&& buffer, int flags = 0);

  /**
   * Creates a queued sender that receives bytes from an owned TCP socket.
   */
  template <class Buffer>
  [[nodiscard]] auto async_receive(tcp_socket& socket, Buffer&& buffer,
                                   int flags = 0);

  /**
   * Creates a direct-submission sender that receives bytes from a non-owning
   * stream socket view.
   */
  template <class Buffer>
  [[nodiscard]] auto async_receive_direct(async_io::stream_socket_view socket,
                                          Buffer&& buffer, int flags = 0);

  /**
   * Creates a direct-submission sender that receives bytes from an owned TCP
   * socket.
   */
  template <class Buffer>
  [[nodiscard]] auto async_receive_direct(tcp_socket& socket, Buffer&& buffer,
                                          int flags = 0);

  /**
   * Creates a queued sender that sends bytes through a non-owning stream socket
   * view.
   */
  template <class Buffer>
  [[nodiscard]] auto async_send(async_io::stream_socket_view socket,
                                Buffer&& buffer, int flags = 0);

  /**
   * Creates a queued sender that sends bytes through an owned TCP socket.
   */
  template <class Buffer>
  [[nodiscard]] auto async_send(tcp_socket& socket, Buffer&& buffer,
                                int flags = 0);

  /**
   * Creates a direct-submission sender that sends bytes through a non-owning
   * stream socket view.
   */
  template <class Buffer>
  [[nodiscard]] auto async_send_direct(async_io::stream_socket_view socket,
                                       Buffer&& buffer, int flags = 0);

  /**
   * Creates a direct-submission sender that sends bytes through an owned TCP
   * socket.
   */
  template <class Buffer>
  [[nodiscard]] auto async_send_direct(tcp_socket& socket, Buffer&& buffer,
                                       int flags = 0);

  /**
   * Creates a queued sender that accepts one connection from a non-owning
   * listening socket view.
   */
  [[nodiscard]] auto async_accept(async_io::listening_socket_view socket,
                                  int flags = 0);

  /**
   * Creates a queued sender that accepts one connection from an owned TCP
   * acceptor.
   */
  [[nodiscard]] auto async_accept(tcp_acceptor& acceptor, int flags = 0);

  /**
   * Creates a direct-submission sender that accepts one connection from a
   * non-owning listening socket view.
   */
  [[nodiscard]] auto async_accept_direct(async_io::listening_socket_view socket,
                                         int flags = 0);

  /**
   * Creates a direct-submission sender that accepts one connection from an
   * owned TCP acceptor.
   */
  [[nodiscard]] auto async_accept_direct(tcp_acceptor& acceptor, int flags = 0);

  /**
   * Creates a queued sender that connects a non-owning stream socket view.
   */
  [[nodiscard]] auto async_connect(async_io::stream_socket_view socket,
                                   const ip::endpoint& endpoint);

  /**
   * Creates a queued sender that connects an owned TCP socket.
   */
  [[nodiscard]] auto async_connect(tcp_socket& socket,
                                   const ip::endpoint& endpoint);

  /**
   * Creates a direct-submission sender that connects a non-owning stream socket
   * view.
   */
  [[nodiscard]] auto async_connect_direct(async_io::stream_socket_view socket,
                                          const ip::endpoint& endpoint);

  /**
   * Creates a direct-submission sender that connects an owned TCP socket.
   */
  [[nodiscard]] auto async_connect_direct(tcp_socket& socket,
                                          const ip::endpoint& endpoint);

  /**
   * Creates a sender that performs an SSL/TLS handshake on an SSL stream.
   */
  template <class NextLayer>
  [[nodiscard]] auto async_handshake(ssl_stream<NextLayer>& stream,
                                     ssl_handshake_type type);

  /**
   * Creates a direct-submission sender that performs an SSL/TLS handshake.
   */
  template <class NextLayer>
  [[nodiscard]] auto async_handshake_direct(ssl_stream<NextLayer>& stream,
                                            ssl_handshake_type type);

  /**
   * Creates a sender that receives decrypted bytes from an SSL stream.
   */
  template <class NextLayer, class Buffer>
  [[nodiscard]] auto async_receive(ssl_stream<NextLayer>& stream,
                                   Buffer&& buffer, int flags = 0);

  /**
   * Creates a direct-submission sender that receives decrypted bytes from an
   * SSL stream.
   */
  template <class NextLayer, class Buffer>
  [[nodiscard]] auto async_receive_direct(ssl_stream<NextLayer>& stream,
                                          Buffer&& buffer, int flags = 0);

  /**
   * Creates a sender that sends encrypted bytes through an SSL stream.
   */
  template <class NextLayer, class Buffer>
  [[nodiscard]] auto async_send(ssl_stream<NextLayer>& stream, Buffer&& buffer,
                                int flags = 0);

  /**
   * Creates a direct-submission sender that sends encrypted bytes through an
   * SSL stream.
   */
  template <class NextLayer, class Buffer>
  [[nodiscard]] auto async_send_direct(ssl_stream<NextLayer>& stream,
                                       Buffer&& buffer, int flags = 0);

  /**
   * Creates a sender that shuts down an SSL stream.
   */
  template <class NextLayer>
  [[nodiscard]] auto async_shutdown(ssl_stream<NextLayer>& stream);

  /**
   * Creates a direct-submission sender that shuts down an SSL stream.
   */
  template <class NextLayer>
  [[nodiscard]] auto async_shutdown_direct(ssl_stream<NextLayer>& stream);

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
  void post(operation_base& operation) noexcept;

 private:
  friend class steady_timer;
  friend class detail::timer_operation_base;
  template <class Receiver>
  friend class detail::timer_wait_operation;

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

  [[nodiscard]] std::error_code flush_operations(
      operation_base* operations) noexcept;

  [[nodiscard]] operation_base* take_pending_io() noexcept;

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

  mutable std::mutex queue_mutex_;
  operation_base* pending_io_head_ = nullptr;
  std::size_t pending_io_count_ = 0;

  timer_state_data timers_;
  steady_timer queued_io_flush_timer_;
};

}  // namespace bupp

#include <bupp/io_context_cpo.h>
#include <bupp/linux/detail/io_context_native_io.h>

#endif  // BUPP_LINUX_IO_CONTEXT_H_
