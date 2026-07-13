#pragma once
#ifndef BUPP_ASYNC_IO_BSD_KQUEUE_HELPER_H_
#define BUPP_ASYNC_IO_BSD_KQUEUE_HELPER_H_

#include <bupp/base/bsd/event.h>
#include <bupp/export.h>
#include <poll.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace bupp::async_io::bsd_native {

/**
 * Native action selected for a kqueue operation.
 */
enum class kqueue_task : std::uint8_t {
  none,
  nop,
  read,
  write,
  poll,
};

/**
 * Fills the kqueue registrations required by one asynchronous operation.
 *
 * A helper is intentionally a short-lived preparation object. It owns at most
 * two kevent values because a poll request can wait for read and write
 * readiness at the same time.
 */
class BUPP_EXPORT kqueue_helper {
 public:
  /** Creates an empty helper. */
  kqueue_helper() noexcept = default;

  /** Prepares an operation that completes without a native registration. */
  void prep_nop() noexcept;

  /** Prepares one EVFILT_READ registration. */
  void prep_read(int descriptor) noexcept;

  /** Prepares one EVFILT_WRITE registration. */
  void prep_write(int descriptor) noexcept;

  /**
   * Prepares the EVFILT_READ and/or EVFILT_WRITE registrations represented by
   * a poll(2) event mask.
   */
  void prep_poll_add(int descriptor, unsigned poll_mask) noexcept;

  /** Alias for prep_poll_add. */
  void prep_poll(int descriptor, unsigned poll_mask) noexcept {
    prep_poll_add(descriptor, poll_mask);
  }

  /** Returns the selected task. */
  [[nodiscard]] kqueue_task task() const noexcept { return task_; }

  /** Returns the prepared descriptor, or -1 if no descriptor is needed. */
  [[nodiscard]] int descriptor() const noexcept { return descriptor_; }

  /** Returns the original poll mask for a poll task. */
  [[nodiscard]] unsigned poll_mask() const noexcept { return poll_mask_; }

  /** Returns the prepared event array. */
  [[nodiscard]] const bupp::base::event* events() const noexcept {
    return events_.data();
  }

  /** Returns the number of prepared events. */
  [[nodiscard]] std::size_t event_count() const noexcept {
    return event_count_;
  }

  /** Returns the prepared event at an index. */
  [[nodiscard]] const bupp::base::event& event(
      std::size_t index = 0) const noexcept {
    return events_[index];
  }

  /** Associates every prepared event with an operation pointer. */
  void set_udata(void* data) noexcept;

  /** Returns zero, or a negative errno produced while preparing the task. */
  [[nodiscard]] int error() const noexcept { return error_; }

 private:
  void reset(kqueue_task task, int descriptor, unsigned poll_mask) noexcept;
  void append_filter(std::int16_t filter) noexcept;

  std::array<bupp::base::event, 2> events_{};
  std::size_t event_count_ = 0;
  kqueue_task task_ = kqueue_task::none;
  int descriptor_ = -1;
  unsigned poll_mask_ = 0;
  int error_ = 0;
};

}  // namespace bupp::async_io::bsd_native

#endif  // BUPP_ASYNC_IO_BSD_KQUEUE_HELPER_H_
