/**
 * @file poll.h
 * @brief Linux native poll I/O operations.
 */

#ifndef BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_POLL_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_POLL_H_

#include <algorithm>
#include <system_error>

namespace bnio::detail {

class poll_model {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, unsigned), bexec::set_stopped_t()>;

  poll_model(async_io::descriptor_view descriptor, unsigned poll_mask) noexcept
      : request_(descriptor, poll_mask) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    request_.prepare(sqe);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<unsigned>(std::max(0, result)));
  }

 private:
  async_io::linux_native::io_uring_poll_request request_;
};

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_POLL_H_
