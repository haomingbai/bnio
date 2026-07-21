/**
 * @file stream_operations.h
 * @brief ssl_stream async operation sender factories.
 */

#pragma once
#ifndef BNIO_SSL_STREAM_OPERATIONS_H_
#define BNIO_SSL_STREAM_OPERATIONS_H_

#include <bnio/detail/ssl/async_operations.h>
#include <bnio/ssl/stream_class.h>

#include <type_traits>
#include <utility>

namespace bnio {

template <class NextLayer>
template <class Scheduler>
auto ssl_stream<NextLayer>::async_handshake(Scheduler scheduler,
                                            ssl_handshake_type type) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::ssl_handshake_sender<scheduler_type, NextLayer>(
      std::move(scheduler), *this, type);
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_read(Scheduler scheduler, Buffer&& buffer,
                                       int) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_read_sender<scheduler_type, NextLayer, holder_type>(
      std::move(scheduler), *this, std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_read_some(Scheduler scheduler,
                                            Buffer&& buffer, int) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_read_sender<scheduler_type, NextLayer, holder_type>(
      std::move(scheduler), *this, std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_write(Scheduler scheduler, Buffer&& buffer,
                                        int) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_write_sender<scheduler_type, NextLayer, holder_type, true>(
      std::move(scheduler), *this, std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_write_some(Scheduler scheduler,
                                             Buffer&& buffer, int) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_write_sender<scheduler_type, NextLayer, holder_type,
                                  false>(std::move(scheduler), *this,
                                         std::move(holder));
}

template <class NextLayer>
template <class Scheduler>
auto ssl_stream<NextLayer>::async_shutdown(Scheduler scheduler) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::ssl_shutdown_sender<scheduler_type, NextLayer>(
      std::move(scheduler), *this);
}

}  // namespace bnio

#endif  // BNIO_SSL_STREAM_OPERATIONS_H_
