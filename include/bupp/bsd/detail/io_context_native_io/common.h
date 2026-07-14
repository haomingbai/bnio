#ifndef BUPP_BSD_DETAIL_IO_CONTEXT_NATIVE_IO_COMMON_H_
#ifndef BUPP_BSD_IO_CONTEXT_H_
#include <bupp/bsd/io_context.h>
#else
#define BUPP_BSD_DETAIL_IO_CONTEXT_NATIVE_IO_COMMON_H_

namespace bupp::detail {

template <class Receiver>
[[nodiscard]] bool stop_requested(const Receiver& receiver) noexcept {
  auto environment = bexec::get_env(receiver);
  auto token = bexec::query(environment, bexec::get_stop_token);
  return token.stop_requested();
}

}  // namespace bupp::detail

#endif  // BUPP_BSD_IO_CONTEXT_H_
#endif  // BUPP_BSD_DETAIL_IO_CONTEXT_NATIVE_IO_COMMON_H_
