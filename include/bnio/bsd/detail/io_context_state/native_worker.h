#ifndef BNIO_BSD_DETAIL_IO_CONTEXT_STATE_NATIVE_WORKER_H_
#ifndef BNIO_BSD_IO_CONTEXT_H_
#include <bnio/bsd/io_context.h>
#else
#define BNIO_BSD_DETAIL_IO_CONTEXT_STATE_NATIVE_WORKER_H_

namespace bnio::detail {

/** @cond BNIO_DETAIL */

struct native_worker {
  native_worker(
      io_context& owner,
      const async_io::bsd_native::kqueue_context_options& options) noexcept
      : owner(&owner), context(options) {}

  io_context* owner = nullptr;
  native_worker* next = nullptr;
  async_io::bsd_native::kqueue_context context;
};

/** @endcond */

}  // namespace bnio::detail

#endif  // BNIO_BSD_IO_CONTEXT_H_
#endif  // BNIO_BSD_DETAIL_IO_CONTEXT_STATE_NATIVE_WORKER_H_
