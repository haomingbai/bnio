/**
 * @file options.h
 * @brief io_context options type.
 */

#pragma once
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_OPTIONS_H_
#define BNIO_DETAIL_POSIX_IO_CONTEXT_OPTIONS_H_

#include <bnio/detail/posix/io_context/native_context.h>

#include <cstdint>

namespace bnio {

/**
 * Native-context options selected for the high-level io_context build.
 */
using platform_io_context_options = detail::native_context_options;

/**
 * Options used to construct a high-level io_context.
 */
struct io_context_options {
  /**
   * Advisory number of expected concurrent run-loop workers; no native
   * workers are pre-created, each run() registers its own lazily.
   */
  std::uint32_t concurrency_hint = 1;

  /**
   * Enables eager immediate completion. When true (default), start() tries
   * a non-blocking operation first and registers with the native poller
   * only if not ready; when false it registers directly. Read once at
   * construction; immutable afterwards.
   */
  bool enable_immediate_io = true;

  /**
   * Native-context options for the configured backend.
   */
  platform_io_context_options platform{};
};

}  // namespace bnio

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_OPTIONS_H_
