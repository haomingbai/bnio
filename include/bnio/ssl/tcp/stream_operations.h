/**
 * @file stream_operations.h
 * @brief SSL stream async operation sender factories.
 */

#pragma once
#ifndef BNIO_SSL_TCP_STREAM_OPERATIONS_H_
#define BNIO_SSL_TCP_STREAM_OPERATIONS_H_

#include <bnio/ssl/detail/async_operations.h>
#include <bnio/ssl/detail/buffers.h>
#include <bnio/ssl/tcp/stream_class.h>

#include <type_traits>
#include <utility>

namespace bnio::ssl::tcp {

template <class NextLayer>
template <class Scheduler>
auto stream<NextLayer>::async_handshake(Scheduler scheduler,
                                        bnio::ssl::handshake_type type) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::handshake_sender<scheduler_type, NextLayer>(
      std::move(scheduler), *this, type);
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto stream<NextLayer>::async_read(Scheduler scheduler, Buffer&& buffer,
                                   int) {
  auto holder = detail::make_read_buffer(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::read_sender<scheduler_type, NextLayer, holder_type>(
      std::move(scheduler), *this, std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto stream<NextLayer>::async_read_some(Scheduler scheduler,
                                        Buffer&& buffer, int) {
  auto holder = detail::make_read_buffer(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::read_sender<scheduler_type, NextLayer, holder_type>(
      std::move(scheduler), *this, std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto stream<NextLayer>::async_write(Scheduler scheduler, Buffer&& buffer,
                                    int) {
  auto holder = detail::make_write_buffer(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::write_sender<scheduler_type, NextLayer, holder_type, true>(
      std::move(scheduler), *this, std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto stream<NextLayer>::async_write_some(Scheduler scheduler,
                                         Buffer&& buffer, int) {
  auto holder = detail::make_write_buffer(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::write_sender<scheduler_type, NextLayer, holder_type,
                              false>(std::move(scheduler), *this,
                                     std::move(holder));
}

template <class NextLayer>
template <class Scheduler>
auto stream<NextLayer>::async_shutdown(Scheduler scheduler) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::shutdown_sender<scheduler_type, NextLayer>(
      std::move(scheduler), *this);
}

}  // namespace bnio::ssl::tcp

#endif  // BNIO_SSL_TCP_STREAM_OPERATIONS_H_
