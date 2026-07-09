#include <bupp/async_io/linux/io_uring_context.h>

#include <cerrno>

namespace bupp::async_io::linux_native {

int io_uring_context::submit() noexcept {
  assert_running();

  int result = 0;
  {
    auto lock = lock_uring();
    result = submit_locked();
  }

  if (result >= 0) {
    notify_waiters();
  }
  return result;
}

int io_uring_context::submit_locked() noexcept {
  if (!ring_.is_open()) {
    return -EINVAL;
  }
  return ring_.submit();
}

void io_uring_context::assert_running() const noexcept {
#ifndef NDEBUG
  assert(state_.load(std::memory_order_acquire) == context_state::running);
#endif
}

}  // namespace bupp::async_io::linux_native
