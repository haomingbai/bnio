/**
 * @file ring.h
 * @brief RAII wrapper for an io_uring instance.
 */

#pragma once
#ifndef BNIO_BASE_LINUX_RING_H_
#define BNIO_BASE_LINUX_RING_H_

#include <bnio/base/linux/completion_queue_entry.h>
#include <bnio/base/linux/submission_queue_entry.h>
#include <bnio/export.h>
#include <liburing.h>

namespace bnio::base {
class params;
}  // namespace bnio::base

namespace bnio::base {

/**
 * RAII wrapper for an io_uring instance.
 *
 * @see io_uring
 */
class BNIO_EXPORT ring {
 public:
  /**
   * Creates a closed ring wrapper.
   *
   * @see io_uring
   */
  ring() noexcept;

  /**
   * Releases the ring if it is open.
   *
   * @see io_uring_queue_exit
   */
  ~ring() noexcept;

  /**
   * Copy construction is disabled because ring owns an io_uring instance.
   */
  ring(const ring&) = delete;

  /**
   * Copy assignment is disabled because ring owns an io_uring instance.
   */
  ring& operator=(const ring&) = delete;

  /**
   * Moves ownership of an open ring.
   *
   * @see io_uring
   */
  ring(ring&& other) noexcept;

  /**
   * Moves ownership of an open ring.
   *
   * @see io_uring
   */
  ring& operator=(ring&& other) noexcept;

  /**
   * Initializes the ring.
   *
   * @see io_uring_queue_init
   */
  int queue_init(unsigned entries, unsigned flags = 0) noexcept;

  /**
   * Initializes the ring with explicit parameters.
   *
   * @see io_uring_queue_init_params
   */
  int queue_init_params(unsigned entries, params& queue_params) noexcept;

  /**
   * Enables a ring created with IORING_SETUP_R_DISABLED.
   *
   * @see io_uring_enable_rings
   */
  int enable() noexcept;

  /**
   * Releases the ring if it is open.
   *
   * @see io_uring_queue_exit
   */
  void queue_exit() noexcept;

  /**
   * Submits queued SQEs.
   *
   * @see io_uring_submit
   */
  int submit() noexcept;

  /**
   * Submits queued SQEs and waits for completions.
   *
   * @see io_uring_submit_and_wait
   */
  int submit_and_wait(unsigned wait_nr) noexcept;

  /**
   * Returns a submission queue entry wrapper, or a null wrapper if full.
   *
   * @see io_uring_get_sqe
   */
  [[nodiscard]] submission_queue_entry get_sqe() noexcept;

  /**
   * Peeks for a completion queue entry.
   *
   * @see io_uring_peek_cqe
   */
  int peek_cqe(completion_queue_entry& cqe) noexcept;

  /**
   * Waits for a completion queue entry.
   *
   * @see io_uring_wait_cqe
   */
  int wait_cqe(completion_queue_entry& cqe) noexcept;

  /**
   * Waits for a completion queue entry with a timeout.
   *
   * @see io_uring_wait_cqe_timeout
   */
  int wait_cqe_timeout(completion_queue_entry& cqe,
                       __kernel_timespec* timeout) noexcept;

  /**
   * Marks a completion queue entry as seen.
   *
   * @see io_uring_cqe_seen
   */
  void cqe_seen(completion_queue_entry cqe) noexcept;

  /**
   * Consumes up to max_count ready completion queue entries.
   *
   * The callback receives each entry before the CQ head is advanced. It should
   * copy any data it needs before returning.
   *
   * @see io_uring_for_each_cqe
   */
  template <class Handler>
  unsigned consume_ready_cqes(unsigned max_count, Handler&& handler) noexcept {
    if (max_count == 0) {
      return 0;
    }

    unsigned consumed_entries = 0;
    unsigned advanced_entries = 0;
    unsigned head = 0;
    io_uring_cqe* raw_cqe = nullptr;
    io_uring_for_each_cqe(&ring_, head, raw_cqe) {
      handler(completion_queue_entry(raw_cqe));
      advanced_entries += cqe_advance_count(raw_cqe);
      ++consumed_entries;
      if (consumed_entries == max_count) {
        break;
      }
    }

    io_uring_cq_advance(&ring_, advanced_entries);
    return consumed_entries;
  }

  /**
   * Returns the wrapped ring.
   *
   * @see io_uring
   */
  [[nodiscard]] io_uring* raw() noexcept;

  /**
   * Returns the wrapped ring.
   *
   * @see io_uring
   */
  [[nodiscard]] const io_uring* raw() const noexcept;

  /**
   * Returns the io_uring file descriptor.
   *
   * @see io_uring
   */
  [[nodiscard]] int native_fd() const noexcept;

  /**
   * Waits on an io_uring file descriptor until at least wait_nr completion
   * events are available.
   *
   * @see io_uring_enter
   */
  static int wait_cqe_event(int ring_fd, unsigned wait_nr) noexcept;

  /**
   * Returns whether this wrapper currently owns an open ring.
   *
   * @see io_uring
   */
  [[nodiscard]] bool is_open() const noexcept;

 private:
  [[nodiscard]] static unsigned cqe_advance_count(
      const io_uring_cqe* cqe) noexcept {
#if defined(IORING_CQE_F_32)
    return (cqe->flags & IORING_CQE_F_32) != 0 ? 2U : 1U;
#else
    (void)cqe;
    return 1U;
#endif
  }

  io_uring ring_{};
  bool open_ = false;
};

}  // namespace bnio::base

#endif  // BNIO_BASE_LINUX_RING_H_
