#pragma once
#ifndef BUPP_BSD_DETAIL_IO_CONTEXT_TIMER_TYPES_H_
#define BUPP_BSD_DETAIL_IO_CONTEXT_TIMER_TYPES_H_

#include <bupp/async_io/bsd/kqueue_context.h>
#include <bupp/async_io/time.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bupp {

class io_context;
class steady_timer;

namespace detail {

/** @cond BUPP_DETAIL */

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

class timer_operation_base
    : public async_io::bsd_native::kqueue_operation_base {
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

class timer_driver_operation
    : public async_io::bsd_native::kqueue_operation_base {
 public:
  explicit timer_driver_operation(io_context& context) noexcept;

  void execute() noexcept override;

 private:
  io_context* context_;
};

struct timer_state_data {
  io_context* owner = nullptr;

  enum class queued_operation_state {
    idle,
    posted,
  };

  [[nodiscard]] bool queue_driver() noexcept;

  void complete_driver() noexcept;

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

}  // namespace bupp

#endif  // BUPP_BSD_DETAIL_IO_CONTEXT_TIMER_TYPES_H_
