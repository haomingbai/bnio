#pragma once
#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_TIMER_TYPES_H_
#define BUPP_LINUX_DETAIL_IO_CONTEXT_TIMER_TYPES_H_

#include <bupp/async_io/linux/io_uring_context.h>
#include <bupp/async_io/time.h>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace bupp {

class io_context;
class steady_timer;

namespace detail {

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

}  // namespace bupp

#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_TIMER_TYPES_H_
