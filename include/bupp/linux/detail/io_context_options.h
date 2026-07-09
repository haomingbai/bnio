#pragma once
#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_OPTIONS_H_
#define BUPP_LINUX_DETAIL_IO_CONTEXT_OPTIONS_H_

#include <bupp/async_io/linux/io_uring_context.h>
#include <bupp/async_io/time.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace bupp {

/**
 * Linux-specific options for the high-level io_context.
 */
struct linux_io_context_options {
  /**
   * Options passed to the underlying io_uring context.
   */
  async_io::linux_native::io_uring_context_options uring{};

  /**
   * Maximum queued operations before the queue is flushed immediately.
   */
  std::size_t max_queued_io_operations = 64;

  /**
   * Maximum time a queued operation waits before an automatic flush.
   */
  async_io::duration queued_io_flush_after = std::chrono::milliseconds(1);
};

/**
 * Platform-specific io_context options for the current build target.
 */
using platform_io_context_options = linux_io_context_options;

/**
 * Options used to construct a high-level io_context.
 */
struct io_context_options {
  /**
   * Requested concurrency hint for future platform implementations.
   */
  std::uint32_t concurrency_hint = 1;

  /**
   * Platform-specific options.
   */
  platform_io_context_options platform{};
};

/**
 * Submission policy for an asynchronous I/O operation.
 */
enum class submit_mode {
  /**
   * Queue the operation and submit it during a later flush.
   */
  queued,

  /**
   * Submit the operation immediately.
   */
  direct,
};

}  // namespace bupp

#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_OPTIONS_H_
