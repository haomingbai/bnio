/**
 * @file local.cpp
 * @brief Local (AF_UNIX) socket and acceptor RAII operations.
 */

#include <bnio/async_io/local/protocol.h>
#include <bnio/detail/error_code.h>
#include <bnio/local/acceptor.h>
#include <bnio/local/socket.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <utility>

namespace bnio::local {

namespace {

[[nodiscard]] std::error_code make_errno_error(int value) noexcept {
  return std::error_code(value, std::generic_category());
}

[[nodiscard]] std::error_code close_fd(int fd) noexcept {
  if (fd < 0) {
    return bnio::detail::empty_error_code;
  }
  if (::close(fd) == 0) {
    return bnio::detail::empty_error_code;
  }
  return make_errno_error(errno);
}

#if !defined(SOCK_CLOEXEC) || !defined(SOCK_NONBLOCK)
// Fallback for platforms without the combined socket() flags: applies
// close-on-exec and non-blocking mode by hand, closing on failure.
[[nodiscard]] int set_nonblocking_cloexec(int descriptor) noexcept {
  if (descriptor < 0) {
    return -1;
  }
#if defined(SOCK_CLOEXEC)
  if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) {
    const int error = errno;
    (void)::close(descriptor);
    errno = error;
    return -1;
  }
#endif
#if defined(SOCK_NONBLOCK)
  if (::fcntl(descriptor, F_SETFL,
              ::fcntl(descriptor, F_GETFL, 0) | O_NONBLOCK) != 0) {
    const int error = errno;
    (void)::close(descriptor);
    errno = error;
    return -1;
  }
#endif
  return descriptor;
}
#endif

// Socket creation shared by the stream and datagram owners: the protocol
// tag supplies the socket(2) triple, and the descriptor comes out in
// non-blocking mode so the kqueue backend can skip per-operation fcntl
// calls.
[[nodiscard]] int open_socket(
    const async_io::local::stream_protocol&) noexcept {
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
  return ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#else
  return set_nonblocking_cloexec(::socket(AF_UNIX, SOCK_STREAM, 0));
#endif
}

[[nodiscard]] int open_socket(
    const async_io::local::datagram_protocol&) noexcept {
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
  return ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#else
  return set_nonblocking_cloexec(::socket(AF_UNIX, SOCK_DGRAM, 0));
#endif
}

// Establishes the nonblocking invariant for a descriptor taken over via
// assign(), mirroring the tcp/udp owners.
void set_nonblocking(int fd) noexcept {
  if (fd >= 0) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0 && (flags & O_NONBLOCK) == 0) {
      (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
  }
}

}  // namespace

stream_socket::~stream_socket() noexcept { (void)close(); }

stream_socket::stream_socket(stream_socket&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

stream_socket& stream_socket::operator=(stream_socket&& other) noexcept {
  if (this != &other) {
    (void)close();
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

std::error_code stream_socket::open(
    async_io::local::stream_protocol protocol) noexcept {
  if (is_open()) {
    return bnio::detail::empty_error_code;
  }
  const int fd = open_socket(protocol);
  if (fd < 0) {
    return make_errno_error(errno);
  }
  fd_ = fd;
  return bnio::detail::empty_error_code;
}

std::error_code stream_socket::close() noexcept {
  const int fd = std::exchange(fd_, -1);
  return close_fd(fd);
}

stream_socket::native_handle_type stream_socket::release() noexcept {
  return std::exchange(fd_, -1);
}

void stream_socket::assign(native_handle_type fd) noexcept {
  if (fd_ != fd) {
    (void)close();
    fd_ = fd;
  }
  set_nonblocking(fd_);
}

std::error_code stream_socket::shutdown(int how) noexcept {
  return view().shutdown(how);
}

std::error_code stream_socket::remote_endpoint(
    async_io::local::endpoint& endpoint) const noexcept {
  return view().remote_endpoint(endpoint);
}

stream_acceptor::~stream_acceptor() noexcept { (void)close(); }

stream_acceptor::stream_acceptor(stream_acceptor&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

stream_acceptor& stream_acceptor::operator=(stream_acceptor&& other) noexcept {
  if (this != &other) {
    (void)close();
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

std::error_code stream_acceptor::open(
    async_io::local::stream_protocol protocol) noexcept {
  if (is_open()) {
    return bnio::detail::empty_error_code;
  }
  const int fd = open_socket(protocol);
  if (fd < 0) {
    return make_errno_error(errno);
  }
  fd_ = fd;
  return bnio::detail::empty_error_code;
}

std::error_code stream_acceptor::bind(
    const async_io::local::endpoint& endpoint) noexcept {
  return view().bind(endpoint);
}

std::error_code stream_acceptor::listen(int backlog) noexcept {
  return view().listen(backlog);
}

std::error_code stream_acceptor::close() noexcept {
  const int fd = std::exchange(fd_, -1);
  return close_fd(fd);
}

stream_acceptor::native_handle_type stream_acceptor::release() noexcept {
  return std::exchange(fd_, -1);
}

void stream_acceptor::assign(native_handle_type fd) noexcept {
  if (fd_ != fd) {
    (void)close();
    fd_ = fd;
  }
  set_nonblocking(fd_);
}

datagram_socket::~datagram_socket() noexcept { (void)close(); }

datagram_socket::datagram_socket(datagram_socket&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

datagram_socket& datagram_socket::operator=(datagram_socket&& other) noexcept {
  if (this != &other) {
    (void)close();
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

std::error_code datagram_socket::open(
    async_io::local::datagram_protocol protocol) noexcept {
  if (is_open()) {
    return bnio::detail::empty_error_code;
  }
  const int fd = open_socket(protocol);
  if (fd < 0) {
    return make_errno_error(errno);
  }
  fd_ = fd;
  return bnio::detail::empty_error_code;
}

std::error_code datagram_socket::bind(
    const async_io::local::endpoint& endpoint) noexcept {
  return view().bind(endpoint);
}

std::error_code datagram_socket::connect(
    const async_io::local::endpoint& endpoint) noexcept {
  return view().connect(endpoint);
}

std::error_code datagram_socket::close() noexcept {
  return close_fd(std::exchange(fd_, -1));
}

datagram_socket::native_handle_type datagram_socket::release() noexcept {
  return std::exchange(fd_, -1);
}

void datagram_socket::assign(native_handle_type fd) noexcept {
  if (fd_ != fd) {
    (void)close();
    fd_ = fd;
  }
  set_nonblocking(fd_);
}

std::error_code datagram_socket::shutdown(int how) noexcept {
  return view().shutdown(how);
}

std::error_code datagram_socket::local_endpoint(
    async_io::local::endpoint& endpoint) const noexcept {
  return view().local_endpoint(endpoint);
}

std::error_code datagram_socket::remote_endpoint(
    async_io::local::endpoint& endpoint) const noexcept {
  return view().remote_endpoint(endpoint);
}

}  // namespace bnio::local
