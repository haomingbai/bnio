/**
 * @file common.h
 * @brief Common SSL async operation support.
 */

#pragma once
#ifndef BNIO_SSL_DETAIL_COMMON_H_
#define BNIO_SSL_DETAIL_COMMON_H_

#include <bnio/buffer.h>
// Keep this header self-contained: clang-format sorting once processed it
// before the ssl umbrella header and exposed the missing include below.
#include <bnio/ssl/tcp/stream_class.h>
#include <openssl/ssl.h>
#include <sys/socket.h>

#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cstddef>
#include <limits>
#include <system_error>

namespace bnio::ssl {

/** @cond BNIO_DETAIL */
namespace detail {

template <class NextLayer>
[[nodiscard]] BIO* read_bio(tcp::stream<NextLayer>& stream) noexcept {
  return stream.native_read_bio();
}

template <class NextLayer>
[[nodiscard]] BIO* write_bio(tcp::stream<NextLayer>& stream) noexcept {
  return stream.native_write_bio();
}

/**
 * Returns the native SSL handle behind a stream's non-owning view.
 */
template <class NextLayer>
[[nodiscard]] SSL* native_ssl(const tcp::stream<NextLayer>& stream) noexcept {
  return stream.native_handle().native_handle();
}

enum class resume_action {
  // Resume another SSL attempt after an internal transport step.
  handshake,
  application_read,
  application_write,
  shutdown,
  transport_read,
  finish,
  // Delivered after the hard-failure flush completes: complete the
  // operation with the staged SSL error (see operation_base).
  fail,
  // Delivered after the output flush of a callback-driven handshake pause:
  // the retry operation invokes its factory for the staged pause.
  pause,
};

[[nodiscard]] inline int bounded_int_size(std::size_t size) noexcept {
  constexpr auto max_int =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  return static_cast<int>(size > max_int ? max_int : size);
}

template <class Receiver>
[[nodiscard]] bool stop_requested(const Receiver& receiver) noexcept {
  auto env = bexec::get_env(receiver);
  auto token = bexec::query(env, bexec::get_stop_token);
  return token.stop_requested();
}

template <class Scheduler, class NextLayer>
auto make_transport_read_sender(Scheduler& scheduler,
                                tcp::stream<NextLayer>& stream, void* data,
                                std::size_t size) {
  auto buffer = bnio::buffer(data, size);
  return stream.lowest_layer().async_read_some(scheduler, buffer);
}

template <class Scheduler, class NextLayer>
auto make_transport_write_sender(Scheduler& scheduler,
                                 tcp::stream<NextLayer>& stream,
                                 const void* data, std::size_t size) {
  auto buffer = bnio::buffer(data, size);
  return stream.lowest_layer().async_write_some(scheduler, buffer,
                                                MSG_NOSIGNAL);
}

}  // namespace detail
/** @endcond */

}  // namespace bnio::ssl

#endif  // BNIO_SSL_DETAIL_COMMON_H_
