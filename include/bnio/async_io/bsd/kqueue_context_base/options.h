/**
 * @file options.h
 * @brief kqueue context options.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPTIONS_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPTIONS_H_

#include <cstdint>

namespace bnio::async_io::bsd_native {

/**
 * Options used to initialize a kqueue-backed async I/O context.
 */
struct kqueue_context_options {
  /**
   * Capacity hint retained for parity with io_uring options; clamped to at
   * least 1 but otherwise unused.
   */
  unsigned entries = 256;

  /** Maximum number of ready kevents collected in one batch. */
  unsigned event_batch_window = 64;

  /** Non-blocking event collection rounds before the run loop blocks. */
  unsigned wait_spin_count = 1;

  /** Ready batches at or below this count are always dispatched inline to
   *  the worker's local CPU queue, regardless of local_queue_threshold. */
  unsigned event_inline_completion_threshold = 64;

  /** Upper bound for tasks dispatched to the local queue per ready-tasks
   *  pass; remaining tasks go to the shared CPU queue. 0 means no limit. */
  unsigned local_queue_threshold = 0;

  /** kevent ident for this context's EVFILT_USER trigger (standalone mode). */
  std::uintptr_t wakeup_ident = 1;
};

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPTIONS_H_
