#pragma once
#ifndef BUPP_LINUX_IO_CONTEXT_CLASS_SCOPE_
#error "This header is an io_context class declaration fragment."
#endif

#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_TIMER_MEMBERS_H_
#define BUPP_LINUX_DETAIL_IO_CONTEXT_TIMER_MEMBERS_H_

/** @cond BUPP_DETAIL */

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
    operation_base* operations,
    async_io::linux_native::io_uring_context::uring_lock& lock) noexcept;

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

std::atomic<operation_base*> pending_io_head_{nullptr};
std::atomic<std::size_t> pending_io_count_{0};

timer_state_data timers_;
steady_timer queued_io_flush_timer_;

/** @endcond */

#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_TIMER_MEMBERS_H_
