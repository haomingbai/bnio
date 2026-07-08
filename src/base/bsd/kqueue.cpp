#include <bupp/base/bsd/kqueue.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace bupp::base {

kqueue::kqueue() noexcept = default;

kqueue::~kqueue() noexcept { close(); }

kqueue::kqueue(kqueue&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

kqueue& kqueue::operator=(kqueue&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

int kqueue::open() noexcept {
  close();
  fd_ = ::kqueue();
  if (fd_ < 0) {
    const int error = errno;
    fd_ = -1;
    return -error;
  }
  return 0;
}

void kqueue::close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool kqueue::is_open() const noexcept { return fd_ >= 0; }

int kqueue::native_fd() const noexcept { return fd_; }

int kqueue::control(const event* changelist, int nchanges, event* eventlist,
                    int nevents, const timespec* timeout) noexcept {
  if (fd_ < 0) {
    return -EBADF;
  }

  const struct kevent* raw_changes =
      changelist == nullptr ? nullptr : changelist->raw();
  struct kevent* raw_events = eventlist == nullptr ? nullptr : eventlist->raw();
  const int result =
      ::kevent(fd_, raw_changes, nchanges, raw_events, nevents, timeout);
  if (result < 0) {
    return -errno;
  }
  return result;
}

}  // namespace bupp::base
