/**
 * @file wake_channel.h
 * @brief Linux eventfd-backed wake channel.
 */

#pragma once
#ifndef BNIO_BASE_LINUX_WAKE_CHANNEL_H_
#define BNIO_BASE_LINUX_WAKE_CHANNEL_H_

#include <bnio/export.h>

namespace bnio::base {

/**
 * Cross-thread wake channel backed by eventfd(2); io_context writes, workers
 * IORING_POLL_ADD the read side.
 *
 * @warning One write wakes all pollers (the intended behaviour during stop()).
 */
class BNIO_EXPORT wake_channel {
 public:
  /** Creates a closed wake channel. */
  wake_channel() noexcept;

  /** Closes the eventfd if it is open. */
  ~wake_channel() noexcept;

  wake_channel(const wake_channel&) = delete;
  wake_channel& operator=(const wake_channel&) = delete;

  /** Moves ownership of the eventfd. */
  wake_channel(wake_channel&& other) noexcept;

  /** Moves ownership of the eventfd. */
  wake_channel& operator=(wake_channel&& other) noexcept;

  /**
   * Opens an eventfd in non-blocking, close-on-exec mode.
   *
   * @return 0 on success, or a negative errno on failure.
   */
  int open() noexcept;

  /** Closes the eventfd if it is open. */
  void close() noexcept;

  /** Returns whether the eventfd is open. */
  [[nodiscard]] bool is_open() const noexcept;

  /** Returns the fd workers should register read interest on. */
  [[nodiscard]] int read_fd() const noexcept;

  /**
   * Writes one unit to wake workers; EAGAIN counts as success because they are
   * already awake. Returns 0, or another negative errno.
   */
  int wake() noexcept;

  /**
   * Drains all pending notifications so later edge-triggered polls detect new
   * signals. Returns 0, or a negative errno.
   */
  int drain() noexcept;

 private:
  int fd_ = -1;
};

}  // namespace bnio::base

#endif  // BNIO_BASE_LINUX_WAKE_CHANNEL_H_
