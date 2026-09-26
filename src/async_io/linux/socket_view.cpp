/**
 * @file socket_view.cpp
 * @brief Linux socket view operations (bind, connect, shutdown, etc.).
 */

#include <bnio/async_io/ip/endpoint.h>
#include <bnio/async_io/linux/socket_address.h>
#include <bnio/async_io/socket_view.h>
#include <bnio/base/socket.h>
#include <bnio/detail/error_code.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstddef>
#include <system_error>
namespace bnio::async_io {
namespace {

std::error_code last_error() noexcept {
  return std::error_code(errno, std::generic_category());
}

std::error_code result_to_error_code(int result) noexcept {
  return result == 0 ? bnio::detail::empty_error_code : last_error();
}

std::error_code bind_socket(int descriptor,
                            const ip::endpoint& endpoint) noexcept {
  const linux_native::socket_address address(endpoint);
  return result_to_error_code(
      ::bind(descriptor, address.data(), address.size()));
}

std::error_code connect_socket(int descriptor,
                               const ip::endpoint& endpoint) noexcept {
  const linux_native::socket_address address(endpoint);
  return result_to_error_code(
      ::connect(descriptor, address.data(), address.size()));
}

std::error_code get_socket_endpoint(int descriptor, bool peer,
                                    ip::endpoint& endpoint) noexcept {
  // Stack-allocated byte storage for the native address, parsed into the
  // C++ endpoint object below.
  alignas(sockaddr_storage) std::byte storage[sizeof(sockaddr_storage)]{};
  auto* const address = reinterpret_cast<sockaddr*>(storage);
  socklen_t size = sizeof(storage);
  const int result =
      peer ? bnio::base::peer_address(descriptor, address, &size)
           : bnio::base::local_address(descriptor, address, &size);
  if (result != 0) {
    endpoint.reset();
    return last_error();
  }
  const auto converted = linux_native::make_endpoint(address, size);
  if (!converted.has_value()) {
    endpoint.reset();
    return std::make_error_code(std::errc::address_family_not_supported);
  }
  endpoint = *converted;
  return bnio::detail::empty_error_code;
}

}  // namespace

std::error_code stream_socket_view::bind(
    const ip::endpoint& endpoint) noexcept {
  return bind_socket(native_handle(), endpoint);
}

std::error_code stream_socket_view::listen(int backlog) noexcept {
  return result_to_error_code(::listen(native_handle(), backlog));
}

std::error_code stream_socket_view::connect(
    const ip::endpoint& endpoint) noexcept {
  return connect_socket(native_handle(), endpoint);
}

std::error_code stream_socket_view::shutdown(int how) noexcept {
  return result_to_error_code(::shutdown(native_handle(), how));
}

std::error_code stream_socket_view::set_reuse_address(bool enabled) noexcept {
  const int value = enabled ? 1 : 0;
  return result_to_error_code(::setsockopt(
      native_handle(), SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)));
}

std::error_code stream_socket_view::remote_endpoint(
    ip::endpoint& endpoint) const noexcept {
  return get_socket_endpoint(native_handle(), true, endpoint);
}

std::error_code datagram_socket_view::bind(
    const ip::endpoint& endpoint) noexcept {
  return bind_socket(native_handle(), endpoint);
}

std::error_code datagram_socket_view::connect(
    const ip::endpoint& endpoint) noexcept {
  return connect_socket(native_handle(), endpoint);
}

std::error_code datagram_socket_view::shutdown(int how) noexcept {
  return result_to_error_code(::shutdown(native_handle(), how));
}

std::error_code datagram_socket_view::set_reuse_address(bool enabled) noexcept {
  const int value = enabled ? 1 : 0;
  return result_to_error_code(::setsockopt(
      native_handle(), SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)));
}

std::error_code datagram_socket_view::local_endpoint(
    ip::endpoint& endpoint) const noexcept {
  return get_socket_endpoint(native_handle(), false, endpoint);
}

std::error_code datagram_socket_view::remote_endpoint(
    ip::endpoint& endpoint) const noexcept {
  return get_socket_endpoint(native_handle(), true, endpoint);
}

}  // namespace bnio::async_io
