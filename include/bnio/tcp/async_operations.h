/**
 * @file async_operations.h
 * @brief TCP async operation sender factories.
 */

#pragma once
#ifndef BNIO_TCP_ASYNC_OPERATIONS_H_
#define BNIO_TCP_ASYNC_OPERATIONS_H_

#include <bnio/buffer.h>
#include <bnio/detail/tcp/async_operations.h>
#include <bnio/tcp/acceptor.h>
#include <bnio/tcp/socket.h>

#include <bexec/receiver.hpp>
#include <type_traits>
#include <utility>

namespace bnio {

/** @cond BNIO_DETAIL */
namespace detail {

template <class Scheduler, class Receiver>
void tcp_accept_operation<Scheduler, Receiver>::child_receiver::set_value(
    std::error_code ec, int fd) noexcept {
  bexec::set_value(std::move(operation_->receiver_), ec, tcp::socket(fd));
}

}  // namespace detail
/** @endcond */

namespace tcp {

template <class Scheduler, class Buffer>
auto socket::async_read(Scheduler scheduler, Buffer&& buffer, int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_read_sender<scheduler_type, holder_type, false>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto socket::async_read_some(Scheduler scheduler, Buffer&& buffer, int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_read_sender<scheduler_type, holder_type, true>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto socket::async_write(Scheduler scheduler, Buffer&& buffer, int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_write_sender<scheduler_type, holder_type, false>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto socket::async_write_some(Scheduler scheduler, Buffer&& buffer, int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_write_sender<scheduler_type, holder_type, true>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler>
auto socket::async_connect(Scheduler scheduler, const ip::endpoint& endpoint) {
  return scheduler.async_connect(view(), endpoint);
}

template <class Scheduler>
auto acceptor::async_accept(Scheduler scheduler, int flags) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::tcp_accept_sender<scheduler_type>(std::move(scheduler), view(),
                                                   flags);
}

}  // namespace tcp

}  // namespace bnio

#endif  // BNIO_TCP_ASYNC_OPERATIONS_H_
