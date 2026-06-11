#include <bupp/async_io/linux/socket_address.h>
#include <bupp/async_io/socket_view.h>
#include <sys/socket.h>

#include <cerrno>

namespace bupp::async_io {
namespace {

std::error_code last_error() noexcept {
  return std::error_code(errno, std::generic_category());
}

std::error_code result_to_error_code(int result) noexcept {
  if (result == 0) {
    return {};
  }
  return last_error();
}

std::error_code bind_socket(int fd, const ip::endpoint& endpoint) noexcept {
  const linux_native::socket_address address(endpoint);
  return result_to_error_code(::bind(fd, address.data(), address.size()));
}

std::error_code connect_socket(int fd, const ip::endpoint& endpoint) noexcept {
  const linux_native::socket_address address(endpoint);
  return result_to_error_code(::connect(fd, address.data(), address.size()));
}

std::error_code listen_socket(int fd, int backlog) noexcept {
  return result_to_error_code(::listen(fd, backlog));
}

std::error_code shutdown_socket(int fd, int how) noexcept {
  return result_to_error_code(::shutdown(fd, how));
}

std::error_code set_socket_reuse_address(int fd, bool enabled) noexcept {
  const int value = enabled ? 1 : 0;
  return result_to_error_code(
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)));
}

}  // namespace

std::error_code listening_socket_view::bind(
    const ip::endpoint& endpoint) noexcept {
  return bind_socket(native_handle(), endpoint);
}

std::error_code listening_socket_view::listen(int backlog) noexcept {
  return listen_socket(native_handle(), backlog);
}

std::error_code listening_socket_view::shutdown(int how) noexcept {
  return shutdown_socket(native_handle(), how);
}

std::error_code listening_socket_view::set_reuse_address(
    bool enabled) noexcept {
  return set_socket_reuse_address(native_handle(), enabled);
}

std::error_code stream_socket_view::connect(
    const ip::endpoint& endpoint) noexcept {
  return connect_socket(native_handle(), endpoint);
}

std::error_code stream_socket_view::shutdown(int how) noexcept {
  return shutdown_socket(native_handle(), how);
}

std::error_code stream_socket_view::set_reuse_address(bool enabled) noexcept {
  return set_socket_reuse_address(native_handle(), enabled);
}

}  // namespace bupp::async_io
