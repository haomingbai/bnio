#pragma once
#ifndef BNIO_LINUX_DETAIL_IO_CONTEXT_TIMER_TYPES_H_
#define BNIO_LINUX_DETAIL_IO_CONTEXT_TIMER_TYPES_H_

#include <bnio/async_io/linux/io_uring_context.h>
#include <bnio/async_io/time.h>
#include <bnio/export.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bnio {

namespace base {
class submission_queue_entry;
}  // namespace base

class io_context;
class steady_timer;

namespace detail {

/** @cond BNIO_DETAIL */

class timer_operation_base;
template <class Receiver>
class timer_wait_operation;
class timer_wait_sender;
template <class Model, class Receiver>
class native_io_operation;

enum class timer_completion_kind {
  value,
  stopped,
};

struct timer_heap_item {
  async_io::time_point deadline{};
  std::uint64_t timer_id = 0;
  std::uint64_t generation = 0;
};

struct timer_slot;

class BNIO_EXPORT timer_operation_base
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

  friend class bnio::io_context;

  io_context* timer_context_;
  timer_operation_base* timer_next_ = nullptr;
  timer_completion_kind timer_completion_ = timer_completion_kind::value;
};

// Operations consumed from a timer during cancellation.
struct submitted_timer_operations {
  timer_operation_base* submitted = nullptr;
  timer_operation_base* waiting = nullptr;
};

class timer_wakeup_operation
    : public async_io::linux_native::io_uring_io_operation_base {
 public:
  explicit timer_wakeup_operation(io_context& context) noexcept;

  void set_timeout(async_io::duration timeout) noexcept;

  void prepare(base::submission_queue_entry& sqe) noexcept override;

  void complete_submit_error(int result) noexcept override;

  void execute() noexcept override;

 private:
  io_context* context_;
  async_io::linux_native::detail::timeout_request timeout_;
};

class timer_update_operation
    : public async_io::linux_native::io_uring_io_operation_base {
 public:
  explicit timer_update_operation(io_context& context) noexcept;

  void set_timeout(async_io::duration timeout) noexcept;

  void prepare(base::submission_queue_entry& sqe) noexcept override;

  void complete_submit_error(int result) noexcept override;

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

  [[nodiscard]] bool can_queue_wakeup() const noexcept;

  [[nodiscard]] bool can_queue_update(
      async_io::time_point deadline) const noexcept;

  void complete_wakeup() noexcept;

  void complete_update() noexcept;

  void mark_wakeup_queued(async_io::time_point deadline) noexcept;

  void mark_update_queued(async_io::time_point deadline) noexcept;

  void push_heap(timer_heap_item item) noexcept;

  void pop_heap() noexcept;

  void swap_heap_items(std::size_t first, std::size_t second) noexcept;

  void sift_heap_up(std::size_t index) noexcept;

  void sift_heap_down(std::size_t index) noexcept;

  [[nodiscard]] bool heap_item_less(std::size_t first,
                                    std::size_t second) const noexcept;

  mutable std::mutex mutex;
  std::unordered_map<std::uint64_t, timer_slot*> timers;
  std::vector<timer_heap_item> heap;
  std::uint64_t next_timer_id = 1;
  queued_operation_state driver = queued_operation_state::idle;
  timeout_state timeout = timeout_state::idle;
  async_io::time_point armed_deadline{};
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

/** @endcond */

}  // namespace detail

}  // namespace bnio

#endif  // BNIO_LINUX_DETAIL_IO_CONTEXT_TIMER_TYPES_H_
