#ifndef BNIO_DETAIL_IO_CONTEXT_NATIVE_WORKER_H_
#ifndef BNIO_DETAIL_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_IO_CONTEXT_NATIVE_WORKER_H_

namespace bnio::detail {

/** @cond BNIO_DETAIL */

struct native_worker {
  native_worker(io_context& owner,
                const native_context_options& options) noexcept
      : owner(&owner), context(options) {}

  io_context* owner = nullptr;
  native_worker* next = nullptr;
  native_context context;
};

/** @endcond */

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_IO_CONTEXT_NATIVE_WORKER_H_
