/**
 * @file options.h
 * @brief kqueue context options.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPTIONS_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPTIONS_H_

#include <cstdint>

namespace bnio::async_io::bsd_native {

/** Options used to initialize a kqueue-backed async I/O context. */
struct kqueue_context_options {
  /** Capacity hint; the context reserves two readiness slots per entry. */
  unsigned entries = 256;

  /** Maximum number of ready kevents collected in one batch. */
  unsigned event_batch_window = 64;

  /** Number of non-blocking event collection rounds before parking. */
  unsigned wait_spin_count = 1;

  /** Ready batches up to this size are always kept on the local queue. */
  unsigned event_inline_completion_threshold = 64;

  /** Per-pass local completion budget; zero means unlimited. */
  unsigned local_queue_threshold = 0;

  /** Identifier reserved for the context's EVFILT_USER wakeup. */
  std::uintptr_t wakeup_ident = 1;

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

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_CONTEXT_BASE_OPTIONS_H_
