#include <bupp/async_io/linux/io_uring_context.h>

#include <cerrno>

namespace bupp::async_io::linux_native {

int io_uring_context::submit() noexcept {
  assert_running();
  assert_ring_owner();
  return submit_ring();
}

int io_uring_context::submit_ring() noexcept {
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

void io_uring_context::assert_ring_owner() const noexcept {
#ifndef NDEBUG
  assert(!run_active_.load(std::memory_order_acquire) ||
         current_context_ == this);
#endif
}

}  // namespace bupp::async_io::linux_native
