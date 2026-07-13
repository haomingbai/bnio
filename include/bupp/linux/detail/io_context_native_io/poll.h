#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_POLL_H_
#ifndef BUPP_LINUX_IO_CONTEXT_H_
#include <bupp/linux/io_context.h>
#else
#define BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_POLL_H_

namespace bupp::detail {

class poll_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(unsigned),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  poll_model(async_io::descriptor_view descriptor, unsigned poll_mask) noexcept
      : request_(descriptor, poll_mask) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    request_.prepare(sqe);
  }

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }

  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<unsigned>(result));
  }

 private:
  async_io::linux_native::io_uring_poll_request request_;
};

}  // namespace bupp::detail

#endif  // BUPP_LINUX_IO_CONTEXT_H_
#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_POLL_H_
