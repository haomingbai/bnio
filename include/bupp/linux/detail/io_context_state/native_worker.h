#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_STATE_NATIVE_WORKER_H_
#ifndef BUPP_LINUX_IO_CONTEXT_H_
#include <bupp/linux/io_context.h>
#else
#define BUPP_LINUX_DETAIL_IO_CONTEXT_STATE_NATIVE_WORKER_H_

namespace bupp::detail {

/** @cond BUPP_DETAIL */

struct native_worker {
  explicit native_worker(io_context& owner) noexcept : owner(&owner) {}

  io_context* owner = nullptr;
  std::atomic<native_worker*> next{nullptr};
  std::atomic<async_io::linux_native::io_uring_context*> context{nullptr};
  std::unique_ptr<async_io::linux_native::io_uring_context> owned_context;
};

/** @endcond */

}  // namespace bupp::detail

#endif  // BUPP_LINUX_IO_CONTEXT_H_
#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_STATE_NATIVE_WORKER_H_
