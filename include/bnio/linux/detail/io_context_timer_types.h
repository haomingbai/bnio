#pragma once
#ifndef BNIO_LINUX_DETAIL_IO_CONTEXT_TIMER_TYPES_H_
#define BNIO_LINUX_DETAIL_IO_CONTEXT_TIMER_TYPES_H_

#include <bnio/async_io/linux/io_uring_context.h>
#include <bnio/async_io/time.h>
#include <bnio/export.h>

#include <atomic>
#include <cstddef>
#include <mutex>

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

struct timer_operation_queue {
  timer_operation_base* head = nullptr;
  timer_operation_base* tail = nullptr;
  std::size_t size = 0;
};

struct timer_slot {
  io_context* context = nullptr;
  async_io::time_point expiry{};
  timer_operation_queue submitted;

  // While active, previous is the parent for a first child and the previous
  // sibling otherwise; child and next are the first-child and next-sibling
  // links of the intrusive pairing heap. While inactive, previous and next
  // form the intrusive doubly linked inactive list and child is null.
  timer_slot* previous = nullptr;
  timer_slot* child = nullptr;
  timer_slot* next = nullptr;
  bool active = false;
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

  void push_heap(timer_slot& timer) noexcept;

  [[nodiscard]] timer_slot* pop_heap() noexcept;

  void erase_heap(timer_slot& timer) noexcept;

  void push_inactive(timer_slot& timer) noexcept;

  void erase_inactive(timer_slot& timer) noexcept;

  [[nodiscard]] timer_slot* heap_front() const noexcept;

  [[nodiscard]] async_io::time_point heap_deadline() const noexcept;

  void clear() noexcept;

 private:
  [[nodiscard]] timer_slot* meld(timer_slot* first,
                                 timer_slot* second) noexcept;

  [[nodiscard]] timer_slot* merge_pairs(timer_slot* first) noexcept;

  [[nodiscard]] static bool timer_less(const timer_slot& first,
                                       const timer_slot& second) noexcept;

 public:
  mutable std::mutex mutex;
  timer_slot* heap = nullptr;
  timer_slot* inactive = nullptr;
  std::atomic<queued_operation_state> driver{queued_operation_state::idle};
  timeout_state timeout = timeout_state::idle;
  async_io::time_point armed_deadline{};
};

/** @endcond */

}  // namespace detail

}  // namespace bnio

#endif  // BNIO_LINUX_DETAIL_IO_CONTEXT_TIMER_TYPES_H_
