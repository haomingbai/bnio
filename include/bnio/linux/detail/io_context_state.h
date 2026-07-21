#pragma once
#ifndef BNIO_LINUX_DETAIL_IO_CONTEXT_STATE_H_
#define BNIO_LINUX_DETAIL_IO_CONTEXT_STATE_H_

#include <bnio/linux/detail/io_context_options.h>

#include <atomic>
#include <cstddef>

namespace bnio::detail {

/** @cond BNIO_DETAIL */

struct native_context_state {
  explicit native_context_state(
      const linux_io_context_options& context_options) noexcept
      : options(context_options) {}

  linux_io_context_options options;
};

struct native_worker;

struct native_worker_state {
  std::atomic<native_worker*> head{nullptr};
};

/** @endcond */

}  // namespace bnio::detail

#endif  // BNIO_LINUX_DETAIL_IO_CONTEXT_STATE_H_
