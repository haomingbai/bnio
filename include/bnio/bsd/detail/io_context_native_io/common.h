#ifndef BNIO_BSD_DETAIL_IO_CONTEXT_NATIVE_IO_COMMON_H_
#ifndef BNIO_BSD_IO_CONTEXT_H_
#include <bnio/bsd/io_context.h>
#else
#define BNIO_BSD_DETAIL_IO_CONTEXT_NATIVE_IO_COMMON_H_

namespace bnio::detail {

template <class Receiver>
[[nodiscard]] bool stop_requested(const Receiver& receiver) noexcept {
  auto environment = bexec::get_env(receiver);
  auto token = bexec::query(environment, bexec::get_stop_token);
  return token.stop_requested();
}

}  // namespace bnio::detail

#endif  // BNIO_BSD_IO_CONTEXT_H_
#endif  // BNIO_BSD_DETAIL_IO_CONTEXT_NATIVE_IO_COMMON_H_
