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

/**
 * Options used to initialize an io_uring-backed async I/O context.
 */
struct io_uring_context_options {
  /**
   * Number of submission queue entries requested for the ring.
   *
   * @see io_uring_queue_init
   */
  unsigned entries = 256;

  /**
   * io_uring setup flags passed to the kernel.
   *
   * Defaults to COOP_TASKRUN | SINGLE_ISSUER | R_DISABLED. R_DISABLED lets
   * the run-loop thread enable the ring and become its designated issuer even
   * when another thread constructed the context.
   *
   * The library falls back automatically on EINVAL for older kernels.
   *
   * @see io_uring_queue_init
   * @see docs/design/io_uring-setup.md
   */
  unsigned setup_flags = bnio::base::detail::io_uring_setup_coop_taskrun |
                         bnio::base::detail::io_uring_setup_single_issuer |
                         (bnio::base::detail::io_uring_setup_single_issuer != 0
                              ? bnio::base::detail::io_uring_setup_r_disabled
                              : 0U);

  /**
   * Maximum number of ready CQEs collected in one batch.
   */
  unsigned cqe_batch_window = 64;

  /**
   * Number of non-blocking polling rounds before a run loop parks.
   */
  unsigned wait_spin_count = 4;

  /**
   * Maximum number of CQE completions kept on the local run queue.
   *
   * CQE batches at or below this count are dispatched inline to the
   * thread-local task queue.  Batches between this and
   * local_queue_threshold go to the local queue with a budget check.
   */
  unsigned cqe_inline_completion_threshold = 64;

  /**
   * Upper bound for CQE tasks dispatched to the local queue per run-loop
   * iteration.  When the cumulative local-queue sink exceeds this value,
   * remaining CQEs are published to the shared CPU queue instead.
   *
   * 0 (the default) means no limit.
   */
  unsigned local_queue_threshold = 0;

  /**
   * When true, adds IORING_SETUP_SQPOLL to setup_flags.
   *
   * A kernel thread polls the SQ ring continuously, eliminating
   * io_uring_enter syscalls for submission.  The trade-off is one
   * dedicated CPU core consumed by the kernel poller thread.
   *
   * @see docs/design/io_uring-setup.md
   */
  bool enable_sqpoll = false;

  /**
   * CPU affinity hint for the SQPOLL kernel thread.
   *
   * 0 means no preference.
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
   * Optional non-owning eventfd used to wake the context run loop.
   *
   * A negative value makes the context create and own a private eventfd.
   * Supplying a descriptor lets an embedding layer provide its own eventfd;
   * the caller must keep it valid until queue_exit() or context destruction.
   */
  int event_fd = -1;

  /**
   * Enables single-probe CPU-task stealing from a peer worker.
   *
   * When a worker finds its local queue and the shared queue empty, it may
   * probe one peer worker's local queue under the run-list lock and, if that
   * peer owns CPU tasks, steal the entire batch.  Disabling this (false)
   * avoids run.lock contention and cache-line traffic at the cost of
   * potentially missing tail-balancing opportunities.  The shared MPSC queue
   * already provides fair distribution for the common case.
   */
  bool enable_steal = true;
};

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPTIONS_H_
