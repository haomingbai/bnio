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
   * Advisory number of expected concurrent run-loop workers.
   *
   * The context does not pre-create native workers; every call to run()
   * registers its own native context lazily.
   */
  std::uint32_t concurrency_hint = 1;

  /**
   * Enables eager immediate completion for I/O operations.
   *
   * When true (the default), native I/O operations probe for immediate
   * completion in start(): the backend issues a non-blocking attempt and
   * only registers with the native poller when the resource is not ready.
   * When false, the probe is skipped and the operation registers directly
   * with the native poller, waiting for readiness before performing I/O.
   *
   * The value is immutable after construction: the io_context reads it once
   * at construction time and stores it, so the hot path only performs a
   * single bool read.
   */
  bool enable_immediate_io = true;

  /**
   * Native-context options for the configured backend.
   */
  platform_io_context_options platform{};
};

}  // namespace bnio

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_OPTIONS_H_
