#pragma once
#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_STATE_H_
#define BUPP_LINUX_DETAIL_IO_CONTEXT_STATE_H_

#include <bupp/linux/detail/io_context_options.h>

#include <atomic>
#include <cstddef>

namespace bupp::detail {

/** @cond BUPP_DETAIL */

struct native_context_state {
  explicit native_context_state(
      const linux_io_context_options& context_options,
      async_io::linux_native::io_uring_task_queue_state& global_state) noexcept
      : context(context_options.uring), options(context_options) {
    context.set_global_state(global_state);
  }

  async_io::linux_native::io_uring_context context;
  linux_io_context_options options;
};

struct native_worker;

struct native_worker_state {
  native_worker* head = nullptr;
  std::atomic<native_worker*> round_robin_cursor{nullptr};
  std::atomic<std::size_t> active_count{1};
  std::atomic<std::size_t> next_run{0};
};

/** @endcond */

}  // namespace bupp::detail

#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_STATE_H_
