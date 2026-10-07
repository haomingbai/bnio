/**
 * @file async_operations.h
 * @brief Local (AF_UNIX) async operation sender factories.
 */

#pragma once
#ifndef BNIO_LOCAL_ASYNC_OPERATIONS_H_
#define BNIO_LOCAL_ASYNC_OPERATIONS_H_

#include <bnio/buffer.h>
#include <bnio/detail/local/async_operations.h>
#include <bnio/local/acceptor.h>
#include <bnio/local/socket.h>

#include <bexec/receiver.hpp>
#include <type_traits>
#include <utility>

namespace bnio {

/** @cond BNIO_DETAIL */
namespace detail {

template <class Scheduler, class Receiver>
void local_accept_operation<Scheduler, Receiver>::child_receiver::set_value(
    std::error_code ec, int fd) noexcept {
  bexec::set_value(std::move(operation_->receiver_), ec,
                   local::stream_socket(fd));
}

}  // namespace detail
/** @endcond */

namespace local {

template <class Scheduler, class Buffer>
auto stream_socket::async_read(Scheduler scheduler, Buffer&& buffer,
                               int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::local_read_sender<scheduler_type, holder_type, false>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto stream_socket::async_read_some(Scheduler scheduler, Buffer&& buffer,
                                    int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::local_read_sender<scheduler_type, holder_type, true>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto stream_socket::async_write(Scheduler scheduler, Buffer&& buffer,
                                int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::local_write_sender<scheduler_type, holder_type, false>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto stream_socket::async_write_some(Scheduler scheduler, Buffer&& buffer,
                                     int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::local_write_sender<scheduler_type, holder_type, true>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler>
auto stream_socket::async_connect(Scheduler scheduler,
                                  const async_io::local::endpoint& endpoint) {
  return scheduler.async_connect(view(), endpoint);
}

template <class Scheduler>
auto stream_acceptor::async_accept(Scheduler scheduler, int flags) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::local_accept_sender<scheduler_type>(std::move(scheduler),
                                                     view(), flags);
}

template <class Scheduler, class Buffer>
auto datagram_socket::async_send(Scheduler scheduler, Buffer&& buffer,
                                 int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  return detail::local_send_sender<std::remove_cvref_t<Scheduler>,
                                   decltype(holder), false>(
      std::move(scheduler), view(), std::move(holder), {}, flags);
}

template <class Scheduler, class Buffer>
auto datagram_socket::async_receive(Scheduler scheduler, Buffer&& buffer,
                                    int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  return detail::local_receive_sender<std::remove_cvref_t<Scheduler>,
                                      decltype(holder), false>(
      std::move(scheduler), view(), std::move(holder), nullptr, flags);
}

template <class Scheduler, class Buffer>
auto datagram_socket::async_send_to(Scheduler scheduler, Buffer&& buffer,
                                    const async_io::local::endpoint& endpoint,
                                    int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  return detail::local_send_sender<std::remove_cvref_t<Scheduler>,
                                   decltype(holder), true>(
      std::move(scheduler), view(), std::move(holder), endpoint, flags);
}

template <class Scheduler, class Buffer>
auto datagram_socket::async_receive_from(Scheduler scheduler, Buffer&& buffer,
                                         async_io::local::endpoint& endpoint,
                                         int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  return detail::local_receive_sender<std::remove_cvref_t<Scheduler>,
                                      decltype(holder), true>(
      std::move(scheduler), view(), std::move(holder), &endpoint, flags);
}

}  // namespace local

}  // namespace bnio

#endif  // BNIO_LOCAL_ASYNC_OPERATIONS_H_
