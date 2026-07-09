#pragma once
#ifndef BUPP_SSL_STREAM_OPERATIONS_H_
#define BUPP_SSL_STREAM_OPERATIONS_H_

#include <bupp/detail/ssl/async_operations.h>
#include <bupp/ssl/stream_class.h>

#include <type_traits>
#include <utility>

namespace bupp {

template <class NextLayer>
template <class Scheduler>
auto ssl_stream<NextLayer>::async_handshake(Scheduler scheduler,
                                            ssl_handshake_type type) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::ssl_handshake_sender<scheduler_type, NextLayer, false>(
      std::move(scheduler), *this, type);
}

template <class NextLayer>
template <class Scheduler>
auto ssl_stream<NextLayer>::async_handshake_direct(Scheduler scheduler,
                                                   ssl_handshake_type type) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::ssl_handshake_sender<scheduler_type, NextLayer, true>(
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
  return detail::ssl_read_sender<scheduler_type, NextLayer, holder_type, false>(
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
  return detail::ssl_read_sender<scheduler_type, NextLayer, holder_type, false>(
      std::move(scheduler), *this, std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_read_direct(Scheduler scheduler,
                                              Buffer&& buffer, int) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_read_sender<scheduler_type, NextLayer, holder_type, true>(
      std::move(scheduler), *this, std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_read_some_direct(Scheduler scheduler,
                                                   Buffer&& buffer, int) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_read_sender<scheduler_type, NextLayer, holder_type, true>(
      std::move(scheduler), *this, std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_write(Scheduler scheduler, Buffer&& buffer,
                                        int) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_write_sender<scheduler_type, NextLayer, holder_type, false,
                                  true>(std::move(scheduler), *this,
                                        std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_write_some(Scheduler scheduler,
                                             Buffer&& buffer, int) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_write_sender<scheduler_type, NextLayer, holder_type, false,
                                  false>(std::move(scheduler), *this,
                                         std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_write_direct(Scheduler scheduler,
                                               Buffer&& buffer, int) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_write_sender<scheduler_type, NextLayer, holder_type, true,
                                  true>(std::move(scheduler), *this,
                                        std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_write_some_direct(Scheduler scheduler,
                                                    Buffer&& buffer, int) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_write_sender<scheduler_type, NextLayer, holder_type, true,
                                  false>(std::move(scheduler), *this,
                                         std::move(holder));
}

template <class NextLayer>
template <class Scheduler>
auto ssl_stream<NextLayer>::async_shutdown(Scheduler scheduler) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::ssl_shutdown_sender<scheduler_type, NextLayer, false>(
      std::move(scheduler), *this);
}

template <class NextLayer>
template <class Scheduler>
auto ssl_stream<NextLayer>::async_shutdown_direct(Scheduler scheduler) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::ssl_shutdown_sender<scheduler_type, NextLayer, true>(
      std::move(scheduler), *this);
}

}  // namespace bupp

#endif  // BUPP_SSL_STREAM_OPERATIONS_H_
