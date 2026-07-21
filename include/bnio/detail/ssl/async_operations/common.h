/**
 * @file common.h
 * @brief Common SSL async operation support.
 */

#pragma once
#ifndef BNIO_DETAIL_SSL_ASYNC_OPERATIONS_COMMON_H_
#define BNIO_DETAIL_SSL_ASYNC_OPERATIONS_COMMON_H_

#include <bnio/buffer.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>

#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cstddef>
#include <limits>
#include <system_error>

namespace bnio {

/** @cond BNIO_DETAIL */
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

template <class Scheduler, class NextLayer>
auto ssl_make_transport_read_sender(Scheduler& scheduler,
                                    ssl_stream<NextLayer>& stream, void* data,
                                    std::size_t size) {
  auto buffer = bnio::buffer(data, size);
  return stream.lowest_layer().async_read_some(scheduler, buffer);
}

template <class Scheduler, class NextLayer>
auto ssl_make_transport_write_sender(Scheduler& scheduler,
                                     ssl_stream<NextLayer>& stream,
                                     const void* data, std::size_t size) {
  auto buffer = bnio::buffer(data, size);
  return stream.lowest_layer().async_write_some(scheduler, buffer,
                                                MSG_NOSIGNAL);
}

}  // namespace detail
/** @endcond */

}  // namespace bnio

#endif  // BNIO_DETAIL_SSL_ASYNC_OPERATIONS_COMMON_H_
