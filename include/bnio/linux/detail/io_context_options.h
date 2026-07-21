#pragma once
#ifndef BNIO_LINUX_DETAIL_IO_CONTEXT_OPTIONS_H_
#define BNIO_LINUX_DETAIL_IO_CONTEXT_OPTIONS_H_

#include <bnio/async_io/linux/io_uring_context.h>

#include <cstdint>

namespace bnio {

/**
 * Linux-specific options for the high-level io_context.
 */
struct linux_io_context_options {
  /**
   * Options passed to the underlying io_uring context.
   */
  async_io::linux_native::io_uring_context_options uring{};
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
   * Advisory number of expected concurrent run-loop workers.
   *
   * The context does not pre-create native workers; every call to run()
   * registers its own native context lazily.
   */
  std::uint32_t concurrency_hint = 1;

  /**
   * Platform-specific options.
   */
  platform_io_context_options platform{};
};

}  // namespace bnio

#endif  // BNIO_LINUX_DETAIL_IO_CONTEXT_OPTIONS_H_
