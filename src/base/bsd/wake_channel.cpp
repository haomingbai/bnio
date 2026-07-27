/**
 * @file wake_channel.cpp
 * @brief BSD pipe-backed wake channel implementation.
 */

#include <bnio/base/bsd/wake_channel.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <utility>

namespace bnio::base {

wake_channel::wake_channel() noexcept = default;

wake_channel::~wake_channel() noexcept { close(); }

wake_channel::wake_channel(wake_channel&& other) noexcept
    : read_fd_(std::exchange(other.read_fd_, -1)),
      write_fd_(std::exchange(other.write_fd_, -1)) {}

wake_channel& wake_channel::operator=(wake_channel&& other) noexcept {
  if (this != &other) {
    close();
    read_fd_ = std::exchange(other.read_fd_, -1);
    write_fd_ = std::exchange(other.write_fd_, -1);
  }
  return *this;
}

int wake_channel::open() noexcept {
  close();

  int pipe_fds[2];
  if (::pipe(pipe_fds) < 0) {
    const int error = errno;
    return -error;
  }

  // Set non-blocking on both ends.
  int flags = ::fcntl(pipe_fds[0], F_GETFL, 0);
  if (flags >= 0) {
    (void)::fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
  }
  flags = ::fcntl(pipe_fds[1], F_GETFL, 0);
  if (flags >= 0) {
    (void)::fcntl(pipe_fds[1], F_SETFL, flags | O_NONBLOCK);
  }

  read_fd_ = pipe_fds[0];
  write_fd_ = pipe_fds[1];
  return 0;
}

void wake_channel::close() noexcept {
  if (read_fd_ >= 0) {
    ::close(read_fd_);
    read_fd_ = -1;
  }
  if (write_fd_ >= 0) {
    ::close(write_fd_);
    write_fd_ = -1;
  }
}

bool wake_channel::is_open() const noexcept { return read_fd_ >= 0; }

int wake_channel::read_fd() const noexcept { return read_fd_; }

int wake_channel::write_fd() const noexcept { return write_fd_; }

int wake_channel::wake() noexcept {
  if (write_fd_ < 0) {
    return -EBADF;
  }

  const std::uint64_t value = 1;
  const auto* bytes = reinterpret_cast<const char*>(&value);
  std::size_t offset = 0;
  while (offset < sizeof(value)) {
    const ssize_t result =
        ::write(write_fd_, bytes + offset, sizeof(value) - offset);
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
  if (read_fd_ < 0) {
    return -EBADF;
  }

  for (;;) {
    char buf[64];
    const ssize_t result = ::read(read_fd_, buf, sizeof(buf));
    if (result > 0) {
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
