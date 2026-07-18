#include <bnio/tcp.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <utility>

#include "bnio/async_io/socket_view.h"
#include "bnio/ip.h"
#include "bnio/tcp/acceptor.h"
#include "bnio/tcp/socket.h"

namespace bnio::tcp {

namespace {

[[nodiscard]] std::error_code make_errno_error(int value) noexcept {
  return std::error_code(value, std::generic_category());
}

[[nodiscard]] int protocol_family(ip::tcp protocol) noexcept {
  switch (protocol.version()) {
    case ip::address::version::v4:
      return AF_INET;
    case ip::address::version::v6:
      return AF_INET6;
    case ip::address::version::unspecified:
      break;
  }
  return AF_UNSPEC;
}

[[nodiscard]] std::error_code close_fd(int fd) noexcept {
  if (fd < 0) {
    return {};
  }
  if (::close(fd) == 0) {
    return {};
  }
  return make_errno_error(errno);
}

[[nodiscard]] int open_socket(int family, int type, int protocol) noexcept {
#if defined(SOCK_CLOEXEC)
  return ::socket(family, type | SOCK_CLOEXEC, protocol);
#else
  const int descriptor = ::socket(family, type, protocol);
  if (descriptor < 0) {
    return -1;
  }
  if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) {
    const int error = errno;
    (void)::close(descriptor);
    errno = error;
    return -1;
  }
  return descriptor;
#endif
}

}  // namespace

socket::~socket() noexcept { (void)close(); }

socket::socket(socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

socket& socket::operator=(socket&& other) noexcept {
  if (this != &other) {
    (void)close();
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

std::error_code socket::open(int family) noexcept {
  if (is_open()) {
    return {};
  }
  const int fd = open_socket(family, SOCK_STREAM, 0);
  if (fd < 0) {
    return make_errno_error(errno);
  }
  fd_ = fd;
  return {};
}

std::error_code socket::open(ip::tcp protocol) noexcept {
  const int family = protocol_family(protocol);
  if (family == AF_UNSPEC) {
    return make_errno_error(EAFNOSUPPORT);
  }
  return open(family);
}

std::error_code socket::close() noexcept {
  const int fd = std::exchange(fd_, -1);
  return close_fd(fd);
}

socket::native_handle_type socket::release() noexcept {
  return std::exchange(fd_, -1);
}

void socket::assign(native_handle_type fd) noexcept {
  if (fd_ != fd) {
    (void)close();
    fd_ = fd;
  }
}

std::error_code socket::shutdown(int how) noexcept {
  return view().shutdown(how);
}

std::error_code socket::set_reuse_address(bool enabled) noexcept {
  return view().set_reuse_address(enabled);
}

acceptor::~acceptor() noexcept { (void)close(); }

acceptor::acceptor(acceptor&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

acceptor& acceptor::operator=(acceptor&& other) noexcept {
  if (this != &other) {
    (void)close();
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

std::error_code acceptor::open(int family) noexcept {
  if (is_open()) {
    return {};
  }
  const int fd = open_socket(family, SOCK_STREAM, 0);
  if (fd < 0) {
    return make_errno_error(errno);
  }
  fd_ = fd;
  return {};
}

std::error_code acceptor::open(ip::tcp protocol) noexcept {
  const int family = protocol_family(protocol);
  if (family == AF_UNSPEC) {
    return make_errno_error(EAFNOSUPPORT);
  }
  return open(family);
}

std::error_code acceptor::bind(const ip::endpoint& endpoint) noexcept {
  return view().bind(endpoint);
}

std::error_code acceptor::listen(int backlog) noexcept {
  return view().listen(backlog);
}

std::error_code acceptor::close() noexcept {
  const int fd = std::exchange(fd_, -1);
  return close_fd(fd);
}

acceptor::native_handle_type acceptor::release() noexcept {
  return std::exchange(fd_, -1);
}

void acceptor::assign(native_handle_type fd) noexcept {
  if (fd_ != fd) {
    (void)close();
    fd_ = fd;
  }
}

std::error_code acceptor::shutdown(int how) noexcept {
  return view().shutdown(how);
}

std::error_code acceptor::set_reuse_address(bool enabled) noexcept {
  return view().set_reuse_address(enabled);
}

}  // namespace bnio::tcp
