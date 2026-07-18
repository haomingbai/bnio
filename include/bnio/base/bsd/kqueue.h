#pragma once
#ifndef BNIO_BASE_BSD_KQUEUE_H_
#define BNIO_BASE_BSD_KQUEUE_H_

#include <bnio/base/bsd/event.h>
#include <bnio/export.h>

#include <ctime>

namespace bnio::base {

/**
 * RAII wrapper for a native kqueue descriptor.
 *
 * @see kqueue
 * @see kevent
 */
class BNIO_EXPORT kqueue {
 public:
  /**
   * Creates a closed kqueue wrapper.
   */
  kqueue() noexcept;

  /**
   * Closes the native descriptor if it is open.
   */
  ~kqueue() noexcept;

  kqueue(const kqueue&) = delete;
  kqueue& operator=(const kqueue&) = delete;

  /**
   * Moves ownership of the native descriptor.
   */
  kqueue(kqueue&& other) noexcept;

  /**
   * Moves ownership of the native descriptor.
   */
  kqueue& operator=(kqueue&& other) noexcept;

  /**
   * Opens a native kqueue descriptor.
   *
   * Returns 0 on success, or a negative errno value on failure.
   */
  int open() noexcept;

  /**
   * Closes the native descriptor if it is open.
   */
  void close() noexcept;

  /**
   * Returns whether this wrapper owns an open kqueue descriptor.
   */
  [[nodiscard]] bool is_open() const noexcept;

  /**
   * Returns the owned native descriptor, or -1 when closed.
   */
  [[nodiscard]] int native_fd() const noexcept;

  /**
   * Calls kevent against the owned kqueue descriptor.
   *
   * Returns the native non-negative event count on success, or a negative errno
   * value on failure.
   */
  int control(const event* changelist, int nchanges, event* eventlist,
              int nevents, const timespec* timeout) noexcept;

 private:
  int fd_ = -1;
};

}  // namespace bnio::base

#endif  // BNIO_BASE_BSD_KQUEUE_H_
