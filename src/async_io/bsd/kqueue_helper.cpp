#include <bnio/async_io/bsd/kqueue_context_base/operation_base.h>
#include <bnio/async_io/bsd/kqueue_helper.h>

#include <cerrno>

namespace bnio::async_io::bsd_native {
namespace {

[[nodiscard]] constexpr unsigned read_poll_mask() noexcept {
  unsigned mask = static_cast<unsigned>(POLLIN | POLLPRI);
#if defined(POLLRDNORM)
  mask |= static_cast<unsigned>(POLLRDNORM);
#endif
#if defined(POLLRDBAND)
  mask |= static_cast<unsigned>(POLLRDBAND);
#endif
  return mask;
}

[[nodiscard]] constexpr unsigned write_poll_mask() noexcept {
  unsigned mask = static_cast<unsigned>(POLLOUT);
#if defined(POLLWRNORM)
  mask |= static_cast<unsigned>(POLLWRNORM);
#endif
#if defined(POLLWRBAND)
  mask |= static_cast<unsigned>(POLLWRBAND);
#endif
  return mask;
}

}  // namespace

void kqueue_helper::reset(kqueue_task task, int descriptor,
                          unsigned poll_mask) noexcept {
  events_ = {};
  event_count_ = 0;
  task_ = task;
  descriptor_ = descriptor;
  poll_mask_ = poll_mask;
  error_ = 0;
}

void kqueue_helper::append_filter(std::int16_t filter) noexcept {
  if (event_count_ >= events_.size()) {
    error_ = -E2BIG;
    return;
  }
  events_[event_count_++].set(static_cast<std::uintptr_t>(descriptor_), filter,
                              EV_ADD | EV_ONESHOT, 0, 0, nullptr);
}

void kqueue_helper::prep_nop() noexcept { reset(kqueue_task::nop, -1, 0); }

void kqueue_helper::prep_read(int descriptor) noexcept {
  reset(kqueue_task::read, descriptor, 0);
  append_filter(EVFILT_READ);
}

void kqueue_helper::prep_write(int descriptor) noexcept {
  reset(kqueue_task::write, descriptor, 0);
  append_filter(EVFILT_WRITE);
}

void kqueue_helper::prep_poll_add(int descriptor, unsigned poll_mask) noexcept {
  reset(kqueue_task::poll, descriptor, poll_mask);
  if ((poll_mask & read_poll_mask()) != 0U) {
    append_filter(EVFILT_READ);
  }
  if ((poll_mask & write_poll_mask()) != 0U) {
    append_filter(EVFILT_WRITE);
  }
  if (event_count_ == 0 && error_ == 0) {
    error_ = -EINVAL;
  }
}

void kqueue_helper::set_udata(void* data) noexcept {
  for (std::size_t index = 0; index < event_count_; ++index) {
    events_[index].set_udata(data);
  }
}

}  // namespace bnio::async_io::bsd_native
