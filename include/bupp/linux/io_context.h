#pragma once
#ifndef BUPP_LINUX_IO_CONTEXT_H_
#define BUPP_LINUX_IO_CONTEXT_H_

#include <bupp/async_io/linux/io_uring_context.h>
#include <bupp/async_io/socket_view.h>
#include <bupp/async_io/time.h>
#include <bupp/export.h>
#include <bupp/ip.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <thread>

namespace bupp {

enum class ssl_handshake_type;

template <class NextLayer>
class ssl_stream;

namespace base {
class submission_queue_entry;
}  // namespace base

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
   * resources and timer thread state.
   */
  io_context(io_context&&) = delete;

  /**
   * Move assignment is disabled because the context owns synchronization
   * resources and timer thread state.
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
   * Creates a queued sender that completes after a relative timeout.
   */
  template <class Rep, class Period>
  [[nodiscard]] auto async_wait(std::chrono::duration<Rep, Period> timeout);

  /**
   * Creates a direct-submission sender that completes after a relative timeout.
   */
  template <class Rep, class Period>
  [[nodiscard]] auto async_wait_direct(
      std::chrono::duration<Rep, Period> timeout);

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
  [[nodiscard]] std::error_code flush_operations(
      operation_base* operations) noexcept;

  [[nodiscard]] operation_base* take_pending_io() noexcept;

  void arm_flush_timer() noexcept;

  void cancel_flush_timer() noexcept;

  void timer_loop() noexcept;

  async_io::linux_native::io_uring_context native_context_;
  linux_io_context_options linux_options_{};

  mutable std::mutex queue_mutex_;
  operation_base* pending_io_head_ = nullptr;
  std::size_t pending_io_count_ = 0;

  std::mutex timer_mutex_;
  std::condition_variable timer_cv_;
  bool timer_stop_ = false;
  bool timer_armed_ = false;
  std::uint64_t timer_generation_ = 0;
  time_point timer_deadline_{};
  std::thread timer_thread_;
};

}  // namespace bupp

#include <bupp/io_context_cpo.h>
#include <bupp/linux/detail/io_context_native_io.h>

#endif  // BUPP_LINUX_IO_CONTEXT_H_
