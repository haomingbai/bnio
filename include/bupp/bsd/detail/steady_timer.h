#pragma once
#ifndef BUPP_BSD_DETAIL_STEADY_TIMER_H_
#define BUPP_BSD_DETAIL_STEADY_TIMER_H_

#include <bupp/async_io/time.h>
#include <bupp/bsd/detail/io_context_timer_types.h>
#include <bupp/export.h>

#include <chrono>
#include <cstddef>

namespace bupp {

/**
 * io_context-bound steady-clock timer.
 */
class BUPP_EXPORT steady_timer {
 public:
  /**
   * Clock used to measure timer deadlines.
   */
  using clock = async_io::clock;

  /**
   * Duration type used by timer expiry APIs.
   */
  using duration = async_io::duration;

  /**
   * Time point type used by timer expiry APIs.
   */
  using time_point = async_io::time_point;

  /**
   * Creates a timer bound to a context with an immediate default expiry.
   */
  explicit steady_timer(io_context& context) noexcept;

  /**
   * Creates a timer bound to a context and absolute expiry time.
   */
  steady_timer(io_context& context, time_point expiry) noexcept;

  /**
   * Creates a timer bound to a context and relative expiry duration.
   */
  template <class Rep, class Period>
  steady_timer(io_context& context,
               std::chrono::duration<Rep, Period> expiry_after) noexcept
      : steady_timer(context,
                     clock::now() +
                         std::chrono::duration_cast<duration>(expiry_after)) {}

  /**
   * Cancels pending waits and unregisters the timer from its context.
   */
  ~steady_timer() noexcept;

  /**
   * Copy construction is disabled because timer waits are context-registered.
   */
  steady_timer(const steady_timer&) = delete;

  /**
   * Copy assignment is disabled because timer waits are context-registered.
   */
  steady_timer& operator=(const steady_timer&) = delete;

  /**
   * Moves timer registration and pending wait state.
   */
  steady_timer(steady_timer&& other) noexcept;

  /**
   * Moves timer registration and pending wait state.
   */
  steady_timer& operator=(steady_timer&& other) noexcept;

  /**
   * Returns the context that owns this timer.
   */
  [[nodiscard]] io_context& context() noexcept { return *timer_.context; }

  /**
   * Returns the context that owns this timer.
   */
  [[nodiscard]] const io_context& context() const noexcept {
    return *timer_.context;
  }

  /**
   * Returns the current absolute expiry time.
   */
  [[nodiscard]] time_point expiry() const noexcept;

  /**
   * Sets the absolute expiry and stops pending waits.
   */
  [[nodiscard]] std::size_t expires_at(time_point expiry) noexcept;

  /**
   * Sets the relative expiry and stops pending waits.
   */
  template <class Rep, class Period>
  [[nodiscard]] std::size_t expires_after(
      std::chrono::duration<Rep, Period> expiry_after) noexcept {
    return expires_at(clock::now() +
                      std::chrono::duration_cast<duration>(expiry_after));
  }

  /**
   * Stops pending waits without changing the expiry time.
   */
  [[nodiscard]] std::size_t cancel() noexcept;

  /**
   * Creates a sender that completes when the timer expires or is stopped.
   */
  [[nodiscard]] auto async_wait();

 private:
  friend class io_context;
  friend class detail::timer_operation_base;
  template <class Receiver>
  friend class detail::timer_wait_operation;

  detail::timer_slot timer_;
};

}  // namespace bupp

#endif  // BUPP_BSD_DETAIL_STEADY_TIMER_H_
