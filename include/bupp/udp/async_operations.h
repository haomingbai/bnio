#pragma once
#ifndef BUPP_UDP_ASYNC_OPERATIONS_H_
#define BUPP_UDP_ASYNC_OPERATIONS_H_

#include <bupp/detail/udp/async_operations.h>
#include <bupp/udp/socket.h>

#include <type_traits>
#include <utility>

namespace bupp::udp {

template <class Scheduler, class Buffer>
auto socket::async_send(Scheduler scheduler, Buffer&& buffer, int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  return detail::udp_send_sender<std::remove_cvref_t<Scheduler>,
                                 decltype(holder), false, false>(
      std::move(scheduler), view(), std::move(holder), {}, flags);
}

template <class Scheduler, class Buffer>
auto socket::async_send_direct(Scheduler scheduler, Buffer&& buffer,
                               int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  return detail::udp_send_sender<std::remove_cvref_t<Scheduler>,
                                 decltype(holder), true, false>(
      std::move(scheduler), view(), std::move(holder), {}, flags);
}

template <class Scheduler, class Buffer>
auto socket::async_receive(Scheduler scheduler, Buffer&& buffer, int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  return detail::udp_receive_sender<std::remove_cvref_t<Scheduler>,
                                    decltype(holder), false, false>(
      std::move(scheduler), view(), std::move(holder), nullptr, flags);
}

template <class Scheduler, class Buffer>
auto socket::async_receive_direct(Scheduler scheduler, Buffer&& buffer,
                                  int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  return detail::udp_receive_sender<std::remove_cvref_t<Scheduler>,
                                    decltype(holder), true, false>(
      std::move(scheduler), view(), std::move(holder), nullptr, flags);
}

template <class Scheduler, class Buffer>
auto socket::async_send_to(Scheduler scheduler, Buffer&& buffer,
                           const ip::endpoint& endpoint, int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  return detail::udp_send_sender<std::remove_cvref_t<Scheduler>,
                                 decltype(holder), false, true>(
      std::move(scheduler), view(), std::move(holder), endpoint, flags);
}

template <class Scheduler, class Buffer>
auto socket::async_send_to_direct(Scheduler scheduler, Buffer&& buffer,
                                  const ip::endpoint& endpoint, int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  return detail::udp_send_sender<std::remove_cvref_t<Scheduler>,
                                 decltype(holder), true, true>(
      std::move(scheduler), view(), std::move(holder), endpoint, flags);
}

template <class Scheduler, class Buffer>
auto socket::async_receive_from(Scheduler scheduler, Buffer&& buffer,
                                ip::endpoint& endpoint, int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  return detail::udp_receive_sender<std::remove_cvref_t<Scheduler>,
                                    decltype(holder), false, true>(
      std::move(scheduler), view(), std::move(holder), &endpoint, flags);
}

template <class Scheduler, class Buffer>
auto socket::async_receive_from_direct(Scheduler scheduler, Buffer&& buffer,
                                       ip::endpoint& endpoint, int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  return detail::udp_receive_sender<std::remove_cvref_t<Scheduler>,
                                    decltype(holder), true, true>(
      std::move(scheduler), view(), std::move(holder), &endpoint, flags);
}

}  // namespace bupp::udp

#endif  // BUPP_UDP_ASYNC_OPERATIONS_H_
