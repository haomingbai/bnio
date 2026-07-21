#ifndef BNIO_LINUX_DETAIL_IO_CONTEXT_STATE_NATIVE_WORKER_H_
#ifndef BNIO_LINUX_IO_CONTEXT_H_
#include <bnio/linux/io_context.h>
#else
#define BNIO_LINUX_DETAIL_IO_CONTEXT_STATE_NATIVE_WORKER_H_

namespace bnio::detail {

/** @cond BNIO_DETAIL */

struct native_worker {
  native_worker(
      io_context& owner,
      const async_io::linux_native::io_uring_context_options& options) noexcept
      : owner(&owner), context(options) {}

  io_context* owner = nullptr;
  native_worker* next = nullptr;
  async_io::linux_native::io_uring_context context;
};

/** @endcond */

}  // namespace bnio::detail

#endif  // BNIO_LINUX_IO_CONTEXT_H_
#endif  // BNIO_LINUX_DETAIL_IO_CONTEXT_STATE_NATIVE_WORKER_H_
