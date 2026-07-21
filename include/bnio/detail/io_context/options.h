#pragma once
#ifndef BNIO_DETAIL_IO_CONTEXT_OPTIONS_H_
#define BNIO_DETAIL_IO_CONTEXT_OPTIONS_H_

#include <bnio/detail/io_context/native_context.h>

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
   * Native-context options for the configured backend.
   */
  platform_io_context_options platform{};
};

}  // namespace bnio

#endif  // BNIO_DETAIL_IO_CONTEXT_OPTIONS_H_
