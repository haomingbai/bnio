#pragma once
#ifndef BUPP_BSD_DETAIL_IO_CONTEXT_OPTIONS_H_
#define BUPP_BSD_DETAIL_IO_CONTEXT_OPTIONS_H_

#include <bupp/async_io/bsd/kqueue_context.h>

#include <cstdint>

namespace bupp {

/**
 * BSD-specific options for the high-level io_context.
 */
struct bsd_io_context_options {
  /**
   * Options passed to the underlying kqueue context.
   */
  async_io::bsd_native::kqueue_context_options kqueue{};
};

/**
 * Platform-specific io_context options for the current build target.
 */
using platform_io_context_options = bsd_io_context_options;

/**
 * Options used to construct a high-level io_context.
 */
struct io_context_options {
  /**
   * Number of native run-loop worker slots reserved by the context.
   */
  std::uint32_t concurrency_hint = 1;

  /**
   * Platform-specific options.
   */
  platform_io_context_options platform{};
};

}  // namespace bupp

#endif  // BUPP_BSD_DETAIL_IO_CONTEXT_OPTIONS_H_
