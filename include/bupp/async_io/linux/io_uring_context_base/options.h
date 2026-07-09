#pragma once
#ifndef BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPTIONS_H_
#define BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPTIONS_H_

#include <bupp/base/linux/params.h>
#include <bupp/base/linux/ring.h>

namespace bupp::async_io::linux_native {

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
   * Defaults to COOP_TASKRUN (supported since Linux 5.19) — lets the kernel
   * defer task_work, reducing involuntary context switches.
   *
   * The library falls back automatically on EINVAL for kernels < 5.19.
   *
   * @see io_uring_queue_init
   * @see docs/design/io_uring-setup.md
   */
  unsigned setup_flags = bupp::base::detail::io_uring_setup_coop_taskrun;

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
   * iteration.  When the cumulative local-queue sink exceeds this value
   * the remaining CQEs are published to the global (cross-thread) queue
   * instead.
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
};

}  // namespace bupp::async_io::linux_native

#endif  // BUPP_ASYNC_IO_LINUX_IO_URING_CONTEXT_BASE_OPTIONS_H_
