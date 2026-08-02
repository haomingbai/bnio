/**
 * @file wake_channel.cpp
 * @brief Linux eventfd-backed wake channel implementation.
 */

#include <bnio/base/linux/wake_channel.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <utility>

namespace bnio::base {

wake_channel::wake_channel() noexcept = default;

wake_channel::~wake_channel() noexcept { close(); }

wake_channel::wake_channel(wake_channel&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

wake_channel& wake_channel::operator=(wake_channel&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

int wake_channel::open() noexcept {
  close();
  fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (fd_ < 0) {
    const int error = errno;
    fd_ = -1;
    return -error;
  }
  return 0;
}

void wake_channel::close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool wake_channel::is_open() const noexcept { return fd_ >= 0; }

int wake_channel::read_fd() const noexcept { return fd_; }

int wake_channel::wake() noexcept {
  if (fd_ < 0) {
    return -EBADF;
  }

  const std::uint64_t value = 1;
  const auto* bytes = reinterpret_cast<const char*>(&value);
  std::size_t offset = 0;
  while (offset < sizeof(value)) {
    const ssize_t result = ::write(fd_, bytes + offset, sizeof(value) - offset);
    if (result > 0) {
      offset += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0 && errno == EAGAIN) {
      return 0;
    }
    return result < 0 ? -errno : -EIO;
  }
  return 0;
}

int wake_channel::drain() noexcept {
  if (fd_ < 0) {
    return -EBADF;
  }

  for (;;) {
    std::uint64_t value = 0;
    const ssize_t result = ::read(fd_, &value, sizeof(value));
    if (result == static_cast<ssize_t>(sizeof(value))) {
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0 && errno == EAGAIN) {
      return 0;
    }
    return result < 0 ? -errno : -EIO;
  }
}

}  // namespace bnio::base
