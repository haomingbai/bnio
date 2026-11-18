/**
 * @file options.h
 * @brief io_uring context options.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPTIONS_H_
#define BNIO_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPTIONS_H_

#include <bnio/base/linux/liburing.h>
#include <bnio/base/linux/params.h>
#include <bnio/base/linux/ring.h>

namespace bnio::async_io::linux_native {

/** Options used to initialize an io_uring-backed async I/O context. */
struct io_uring_context_options {
  /** Number of submission queue entries requested for the ring. */
  unsigned entries = 256;

  /**
   * io_uring setup flags passed to the kernel.
   *
   * R_DISABLED lets the run-loop thread enable the ring and become its
   * designated issuer even when another thread constructed the context.
   * EINVAL falls back automatically on older kernels.
   */
  unsigned setup_flags = bnio::base::detail::io_uring_setup_coop_taskrun |
                         bnio::base::detail::io_uring_setup_single_issuer |
                         (bnio::base::detail::io_uring_setup_single_issuer != 0
                              ? bnio::base::detail::io_uring_setup_r_disabled
                              : 0U);

  /** Maximum ready CQEs collected in one batch. */
  unsigned cqe_batch_window = 64;

  /** Non-blocking polling rounds before a run loop parks. */
  unsigned wait_spin_count = 4;

  /**
   * CQE batches at or below this count dispatch inline to the thread-local
   * task queue; larger batches go through the local queue with a budget check
   * (see local_queue_threshold).
   */
  unsigned cqe_inline_completion_threshold = 64;

  /**
   * Per-iteration cap on CQE tasks sunk into the local queue; beyond it,
   * remaining CQEs are published to the shared CPU queue. 0 means no limit.
   */
  unsigned local_queue_threshold = 0;

  /**
   * Adds IORING_SETUP_SQPOLL to setup_flags: a kernel thread polls the SQ
   * continuously, removing submission syscalls at the cost of one dedicated
   * CPU core.
   */
  bool enable_sqpoll = false;

  /** SQPOLL kernel-thread CPU affinity hint; 0 means no preference.
   *
   * @see io_uring_params::sq_thread_cpu
   */
  unsigned sqpoll_thread_cpu = 0;

  /**
   * SQPOLL idle timeout in milliseconds before the kernel thread parks.
   *
   * @see io_uring_params::sq_thread_idle
   */
  unsigned sqpoll_idle_ms = 1000;

  /**
   * Eventfd used to wake the context run loop.
   *
   * Negative: the context creates and owns one. Otherwise it is a borrowed
   * descriptor that must stay valid until queue_exit() or destruction.
   */
  int event_fd = -1;
};

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPTIONS_H_
