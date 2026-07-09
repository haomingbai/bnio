#pragma once
#ifndef BUPP_LINUX_IO_CONTEXT_CLASS_SCOPE_
#error "This header is an io_context class declaration fragment."
#endif

#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_SCHEDULER_MEMBERS_H_
#define BUPP_LINUX_DETAIL_IO_CONTEXT_SCHEDULER_MEMBERS_H_

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
  [[nodiscard]] auto async_read_some_direct(async_io::stream_socket_view socket,
                                            mutable_buffer buffer,
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
                                      const_buffer buffer, int flags = 0) const;

  /**
   * Creates a direct-submission sender that writes the whole buffer to a
   * socket.
   */
  [[nodiscard]] auto async_write_direct(async_io::stream_socket_view socket,
                                        const_buffer buffer,
                                        int flags = 0) const;

  /**
   * Creates a sender for one direct-submission socket write operation without
   * retrying short writes.
   */
  [[nodiscard]] auto async_write_some_direct(
      async_io::stream_socket_view socket, const_buffer buffer,
      int flags = 0) const;

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
  [[nodiscard]] auto async_accept(async_io::listening_socket_view socket,
                                  int flags = 0) const;

  /**
   * Creates a direct-submission sender that accepts one connection.
   */
  [[nodiscard]] auto async_accept_direct(async_io::listening_socket_view socket,
                                         int flags = 0) const;

  /**
   * Creates a queued sender that connects a socket to an endpoint.
   */
  [[nodiscard]] auto async_connect(async_io::stream_socket_view socket,
                                   const ip::endpoint& endpoint) const;

  /**
   * Creates a direct-submission sender that connects a socket to an endpoint.
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

  explicit basic_scheduler(io_context& context) noexcept : context_(&context) {}

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

#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_SCHEDULER_MEMBERS_H_
