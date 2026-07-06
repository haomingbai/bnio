#pragma once
#ifndef BUPP_DETAIL_SSL_ASYNC_OPERATIONS_COMMON_H_
#define BUPP_DETAIL_SSL_ASYNC_OPERATIONS_COMMON_H_

#include <bupp/buffer.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>

#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cstddef>
#include <limits>
#include <system_error>

namespace bupp {

/** @cond BUPP_DETAIL */
namespace detail {

template <class NextLayer>
[[nodiscard]] BIO* read_bio(ssl_stream<NextLayer>& stream) noexcept {
  return stream.native_read_bio();
}

template <class NextLayer>
[[nodiscard]] BIO* write_bio(ssl_stream<NextLayer>& stream) noexcept {
  return stream.native_write_bio();
}

[[nodiscard]] inline std::error_code last_ssl_error() noexcept {
  const unsigned long error = ERR_get_error();
  if (error == 0) {
    return std::make_error_code(std::errc::protocol_error);
  }
  return make_openssl_error(error);
}

enum class ssl_resume_action {
  handshake,
  application_read,
  application_write,
  shutdown,
  transport_read,
  finish,
};

[[nodiscard]] inline int ssl_bounded_int_size(std::size_t size) noexcept {
  constexpr auto max_int =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  return static_cast<int>(size > max_int ? max_int : size);
}

template <class Receiver>
[[nodiscard]] bool ssl_stop_requested(const Receiver& receiver) noexcept {
  auto env = bexec::get_env(receiver);
  auto token = bexec::query(env, bexec::get_stop_token);
  return token.stop_requested();
}

template <bool DirectSubmit, class Scheduler, class NextLayer>
auto ssl_make_transport_read_sender(Scheduler& scheduler,
                                    ssl_stream<NextLayer>& stream, void* data,
                                    std::size_t size) {
  auto buffer = bupp::buffer(data, size);
  if constexpr (DirectSubmit) {
    return stream.lowest_layer().async_read_direct(scheduler, buffer);
  } else {
    return stream.lowest_layer().async_read(scheduler, buffer);
  }
}

template <bool DirectSubmit, class Scheduler, class NextLayer>
auto ssl_make_transport_write_sender(Scheduler& scheduler,
                                     ssl_stream<NextLayer>& stream,
                                     const void* data, std::size_t size) {
  auto buffer = bupp::buffer(data, size);
  if constexpr (DirectSubmit) {
    return stream.lowest_layer().async_write_direct(scheduler, buffer,
                                                    MSG_NOSIGNAL);
  } else {
    return stream.lowest_layer().async_write(scheduler, buffer, MSG_NOSIGNAL);
  }
}

}  // namespace detail
/** @endcond */

}  // namespace bupp

#endif  // BUPP_DETAIL_SSL_ASYNC_OPERATIONS_COMMON_H_
