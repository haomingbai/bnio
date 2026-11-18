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
 * RAII owner for an io_uring instance.
 *
 * Move-only; queue_exit() releases the ring if it is open.
 *
 * @see io_uring
 */
class BNIO_EXPORT ring {
 public:
  /** Creates a closed ring wrapper. */
  ring() noexcept;

  /** Releases the ring if it is open. */
  ~ring() noexcept;

  /** Deleted: the wrapper owns an io_uring instance. */
  ring(const ring&) = delete;

  /** Deleted: the wrapper owns an io_uring instance. */
  ring& operator=(const ring&) = delete;

  /** Moves ownership of the ring. */
  ring(ring&& other) noexcept;

  /** Move assigns ownership of the ring. */
  ring& operator=(ring&& other) noexcept;

  /** Initializes the ring. @see io_uring_queue_init */
  int queue_init(unsigned entries, unsigned flags = 0) noexcept;

  /** Initializes the ring with explicit @p queue_params.
   *  @see io_uring_queue_init_params */
  int queue_init_params(unsigned entries, params& queue_params) noexcept;

  /** Enables a ring created with IORING_SETUP_R_DISABLED.
   *  @see io_uring_enable_rings */
  int enable() noexcept;

  /** Releases the ring if it is open. */
  void queue_exit() noexcept;

  /** Submits queued SQEs. @see io_uring_submit */
  int submit() noexcept;

  /** Submits queued SQEs and waits for completions.
   *  @see io_uring_submit_and_wait */
  int submit_and_wait(unsigned wait_nr) noexcept;

  /** Returns an SQE wrapper, or a null wrapper when the SQ is full. */
  [[nodiscard]] submission_queue_entry get_sqe() noexcept;

  /** Peeks for a CQE. @see io_uring_peek_cqe */
  int peek_cqe(completion_queue_entry& cqe) noexcept;

  /** Waits for a CQE. @see io_uring_wait_cqe */
  int wait_cqe(completion_queue_entry& cqe) noexcept;

  /** Waits for a CQE with a timeout. @see io_uring_wait_cqe_timeout */
  int wait_cqe_timeout(completion_queue_entry& cqe,
                       __kernel_timespec* timeout) noexcept;

  /** Marks a CQE as seen. @see io_uring_cqe_seen */
  void cqe_seen(completion_queue_entry cqe) noexcept;

  /**
   * Consumes up to @p max_count ready CQEs.
   *
   * The handler runs before the CQ head is advanced, so it must copy any data
   * it needs before returning.
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

  /** Returns the wrapped ring. */
  [[nodiscard]] io_uring* raw() noexcept;

  /** Const overload. */
  [[nodiscard]] const io_uring* raw() const noexcept;

  /** Returns the io_uring file descriptor. */
  [[nodiscard]] int native_fd() const noexcept;

  /** Waits on @p ring_fd for at least @p wait_nr completions.
   *  @see io_uring_enter */
  static int wait_cqe_event(int ring_fd, unsigned wait_nr) noexcept;

  /** Returns whether the wrapper owns an open ring. */
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
