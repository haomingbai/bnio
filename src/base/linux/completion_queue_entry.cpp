#include <bupp/base/linux/completion_queue_entry.h>
#include <liburing.h>

#include "bupp/base/linux/liburing.h"

namespace bupp::base {

completion_queue_entry::completion_queue_entry() noexcept = default;

completion_queue_entry::completion_queue_entry(io_uring_cqe* cqe) noexcept
    : cqe_(cqe) {}

io_uring_cqe* completion_queue_entry::raw() noexcept { return cqe_; }

const io_uring_cqe* completion_queue_entry::raw() const noexcept {
  return cqe_;
}

int completion_queue_entry::res() const noexcept { return cqe_->res; }

unsigned completion_queue_entry::flags() const noexcept { return cqe_->flags; }

void* completion_queue_entry::get_data() const noexcept {
  return io_uring_cqe_get_data(cqe_);
}

std::uint64_t completion_queue_entry::get_data64() const noexcept {
  return detail::io_uring_cqe_get_data64(cqe_);
}

}  // namespace bupp::base
