#ifndef BUPP_BSD_DETAIL_IO_CONTEXT_STATE_NATIVE_WORKER_H_
#ifndef BUPP_BSD_IO_CONTEXT_H_
#include <bupp/bsd/io_context.h>
#else
#define BUPP_BSD_DETAIL_IO_CONTEXT_STATE_NATIVE_WORKER_H_

namespace bupp::detail {

/** @cond BUPP_DETAIL */

struct native_worker {
  explicit native_worker(io_context& owner) noexcept : owner(&owner) {}

  io_context* owner = nullptr;
  std::atomic<native_worker*> next{nullptr};
  std::atomic<async_io::bsd_native::kqueue_context*> context{nullptr};
  std::unique_ptr<async_io::bsd_native::kqueue_context> owned_context;
};

/** @endcond */

}  // namespace bupp::detail

#endif  // BUPP_BSD_IO_CONTEXT_H_
#endif  // BUPP_BSD_DETAIL_IO_CONTEXT_STATE_NATIVE_WORKER_H_
