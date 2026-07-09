#pragma once
#ifndef BUPP_TCP_ASYNC_OPERATIONS_H_
#define BUPP_TCP_ASYNC_OPERATIONS_H_

#include <bupp/buffer.h>
#include <bupp/detail/tcp/async_operations.h>
#include <bupp/tcp/acceptor.h>
#include <bupp/tcp/socket.h>

#include <bexec/receiver.hpp>
#include <type_traits>
#include <utility>

namespace bupp {

/** @cond BUPP_DETAIL */
namespace detail {

template <class Scheduler, bool DirectSubmit, class Receiver>
void tcp_accept_operation<Scheduler, DirectSubmit, Receiver>::child_receiver::
    set_value(int fd) noexcept {
  bexec::set_value(std::move(operation_->receiver_), tcp_socket(fd));
}

}  // namespace detail
/** @endcond */

template <class Scheduler, class Buffer>
auto tcp_socket::async_read(Scheduler scheduler, Buffer&& buffer, int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_read_sender<scheduler_type, holder_type, false, false>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto tcp_socket::async_read_some(Scheduler scheduler, Buffer&& buffer,
                                 int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_read_sender<scheduler_type, holder_type, false, true>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto tcp_socket::async_read_direct(Scheduler scheduler, Buffer&& buffer,
                                   int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_read_sender<scheduler_type, holder_type, true, false>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto tcp_socket::async_read_some_direct(Scheduler scheduler, Buffer&& buffer,
                                        int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_read_sender<scheduler_type, holder_type, true, true>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto tcp_socket::async_write(Scheduler scheduler, Buffer&& buffer, int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_write_sender<scheduler_type, holder_type, false, false>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto tcp_socket::async_write_some(Scheduler scheduler, Buffer&& buffer,
                                  int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_write_sender<scheduler_type, holder_type, false, true>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto tcp_socket::async_write_direct(Scheduler scheduler, Buffer&& buffer,
                                    int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_write_sender<scheduler_type, holder_type, true, false>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler, class Buffer>
auto tcp_socket::async_write_some_direct(Scheduler scheduler, Buffer&& buffer,
                                         int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::tcp_write_sender<scheduler_type, holder_type, true, true>(
      std::move(scheduler), view(), std::move(holder), flags);
}

template <class Scheduler>
auto tcp_socket::async_connect(Scheduler scheduler,
                               const ip::endpoint& endpoint) {
  return scheduler.async_connect(view(), endpoint);
}

template <class Scheduler>
auto tcp_socket::async_connect_direct(Scheduler scheduler,
                                      const ip::endpoint& endpoint) {
  return scheduler.async_connect_direct(view(), endpoint);
}

template <class Scheduler>
auto tcp_acceptor::async_accept(Scheduler scheduler, int flags) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::tcp_accept_sender<scheduler_type, false>(std::move(scheduler),
                                                          view(), flags);
}

template <class Scheduler>
auto tcp_acceptor::async_accept_direct(Scheduler scheduler, int flags) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::tcp_accept_sender<scheduler_type, true>(std::move(scheduler),
                                                         view(), flags);
}

}  // namespace bupp

#endif  // BUPP_TCP_ASYNC_OPERATIONS_H_
