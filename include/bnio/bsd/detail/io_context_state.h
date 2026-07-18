#pragma once
#ifndef BNIO_BSD_DETAIL_IO_CONTEXT_STATE_H_
#define BNIO_BSD_DETAIL_IO_CONTEXT_STATE_H_

#include <bnio/bsd/detail/io_context_options.h>

#include <atomic>
#include <cstddef>

namespace bnio::detail {

/** @cond BNIO_DETAIL */

struct native_context_state {
  explicit native_context_state(
      const bsd_io_context_options& context_options,
      async_io::bsd_native::kqueue_task_queue_state& global_state) noexcept
      : context(context_options.kqueue), options(context_options) {
    context.set_global_state(&global_state);
  }

  async_io::bsd_native::kqueue_context context;
  bsd_io_context_options options;
};

struct native_worker;

struct native_worker_state {
  native_worker* head = nullptr;
  std::atomic<native_worker*> round_robin_cursor{nullptr};
  std::atomic<std::size_t> active_count{1};
  std::atomic<std::size_t> next_run{0};
};

/** @endcond */

}  // namespace bnio::detail

#endif  // BNIO_BSD_DETAIL_IO_CONTEXT_STATE_H_
