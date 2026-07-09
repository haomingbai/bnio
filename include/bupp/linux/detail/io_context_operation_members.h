#pragma once
#ifndef BUPP_LINUX_IO_CONTEXT_CLASS_SCOPE_
#error "This header is an io_context class declaration fragment."
#endif

#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_OPERATION_MEMBERS_H_
#define BUPP_LINUX_DETAIL_IO_CONTEXT_OPERATION_MEMBERS_H_

/**
 * Base class for operations scheduled by io_context.
 */
class operation_base : public async_io::linux_native::io_uring_operation_base {
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
   * Intrusive next pointer used by the lock-free pending-I/O stack.
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
  explicit schedule_sender(io_context& context) noexcept : context_(&context) {}

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

#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_OPERATION_MEMBERS_H_
