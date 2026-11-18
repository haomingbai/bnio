/**
 * @file wake_channel.h
 * @brief BSD pipe-backed wake channel.
 */

#pragma once
#ifndef BNIO_BASE_BSD_WAKE_CHANNEL_H_
#define BNIO_BASE_BSD_WAKE_CHANNEL_H_

#include <bnio/export.h>

namespace bnio::base {

/**
 * Cross-thread wake channel backed by pipe(2); io_context writes to the write
 * end, and each worker registers EVFILT_READ | EV_CLEAR on the read end.
 *
 * @warning One write wakes all readers (the intended behaviour during stop()).
 */
class BNIO_EXPORT wake_channel {
 public:
  /** Creates a closed wake channel. */
  wake_channel() noexcept;

  /** Closes both pipe ends if they are open. */
  ~wake_channel() noexcept;

  wake_channel(const wake_channel&) = delete;
  wake_channel& operator=(const wake_channel&) = delete;

  /** Moves ownership of both pipe ends. */
  wake_channel(wake_channel&& other) noexcept;

  /** Moves ownership of both pipe ends. */
  wake_channel& operator=(wake_channel&& other) noexcept;

  /**
   * Opens a pipe with non-blocking read and write ends.
   *
   * @return 0 on success, or a negative errno on failure.
   */
  int open() noexcept;

  /** Closes both pipe ends if they are open. */
  void close() noexcept;

  /** Returns whether the pipe is open. */
  [[nodiscard]] bool is_open() const noexcept;

  /** Returns the read-end fd workers register EVFILT_READ on. */
  [[nodiscard]] int read_fd() const noexcept;

  /** Returns the write-end fd io_context writes to for wake. */
  [[nodiscard]] int write_fd() const noexcept;

  /**
   * Writes one byte to the pipe to wake workers.
   *
   * @return 0 on success or when the pipe is full (EAGAIN is treated as
   *         success), or another negative errno on failure.
   */
  int wake() noexcept;

  /**
   * Drains all pending wake bytes so later edge-triggered EVFILT_READ polls
   * detect new signals. Returns 0, or a negative errno.
   */
  int drain() noexcept;

 private:
  int read_fd_ = -1;
  int write_fd_ = -1;
};

}  // namespace bnio::base

#endif  // BNIO_BASE_BSD_WAKE_CHANNEL_H_
