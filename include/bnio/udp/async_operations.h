#pragma once
#ifndef BNIO_UDP_ASYNC_OPERATIONS_H_
#define BNIO_UDP_ASYNC_OPERATIONS_H_

#include <bnio/detail/udp/async_operations.h>
#include <bnio/udp/socket.h>

#include <type_traits>
#include <utility>

namespace bnio::udp {

template <class Scheduler, class Buffer>
auto socket::async_send(Scheduler scheduler, Buffer&& buffer, int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  return detail::udp_send_sender<std::remove_cvref_t<Scheduler>,
                                 decltype(holder), false>(
      std::move(scheduler), view(), std::move(holder), {}, flags);
}

template <class Scheduler, class Buffer>
auto socket::async_receive(Scheduler scheduler, Buffer&& buffer, int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  return detail::udp_receive_sender<std::remove_cvref_t<Scheduler>,
                                    decltype(holder), false>(
      std::move(scheduler), view(), std::move(holder), nullptr, flags);
}

template <class Scheduler, class Buffer>
auto socket::async_send_to(Scheduler scheduler, Buffer&& buffer,
                           const ip::endpoint& endpoint, int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  return detail::udp_send_sender<std::remove_cvref_t<Scheduler>,
                                 decltype(holder), true>(
      std::move(scheduler), view(), std::move(holder), endpoint, flags);
}

template <class Scheduler, class Buffer>
auto socket::async_receive_from(Scheduler scheduler, Buffer&& buffer,
                                ip::endpoint& endpoint, int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  return detail::udp_receive_sender<std::remove_cvref_t<Scheduler>,
                                    decltype(holder), true>(
      std::move(scheduler), view(), std::move(holder), &endpoint, flags);
}

}  // namespace bnio::udp

#endif  // BNIO_UDP_ASYNC_OPERATIONS_H_
