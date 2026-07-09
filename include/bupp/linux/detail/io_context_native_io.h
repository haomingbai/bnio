#pragma once
#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_H_
#define BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_H_

#include <bupp/linux/detail/io_context_native_io/common.h>
#include <bupp/linux/detail/io_context_native_io/file.h>
#include <bupp/linux/detail/io_context_native_io/poll.h>
#include <bupp/linux/detail/io_context_native_io/socket.h>
#include <bupp/linux/detail/io_context_native_io/timer_wait.h>
#include <bupp/linux/detail/io_context_native_io/write_all.h>

namespace bupp {

/** @cond BUPP_DETAIL */

inline auto io_context::async_read(async_io::stream_socket_view socket,
                                   mutable_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::socket_read_model(socket, buffer, flags),
      submit_mode::queued);
}

inline auto io_context::async_read_some(async_io::stream_socket_view socket,
                                        mutable_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::socket_read_model(socket, buffer, flags),
      submit_mode::queued);
}

inline auto io_context::async_read_direct(async_io::stream_socket_view socket,
                                          mutable_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::socket_read_model(socket, buffer, flags),
      submit_mode::direct);
}

inline auto io_context::async_read_some_direct(
    async_io::stream_socket_view socket, mutable_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::socket_read_model(socket, buffer, flags),
      submit_mode::direct);
}

inline auto io_context::async_write(async_io::stream_socket_view socket,
                                    const_buffer buffer, int flags) {
  return detail::write_all_sender(detail::socket_write_all_state(
      *this, socket, buffer, flags, submit_mode::queued));
}

inline auto io_context::async_write_some(async_io::stream_socket_view socket,
                                         const_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::socket_write_model(socket, buffer, flags),
      submit_mode::queued);
}

inline auto io_context::async_write_direct(async_io::stream_socket_view socket,
                                           const_buffer buffer, int flags) {
  return detail::write_all_sender(detail::socket_write_all_state(
      *this, socket, buffer, flags, submit_mode::direct));
}

inline auto io_context::async_write_some_direct(
    async_io::stream_socket_view socket, const_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::socket_write_model(socket, buffer, flags),
      submit_mode::direct);
}

inline auto io_context::async_read(async_io::descriptor_view descriptor,
                                   mutable_buffer buffer,
                                   std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::read_model(descriptor, buffer, offset),
      submit_mode::queued);
}

inline auto io_context::async_read_some(async_io::descriptor_view descriptor,
                                        mutable_buffer buffer,
                                        std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::read_model(descriptor, buffer, offset),
      submit_mode::queued);
}

inline auto io_context::async_read_direct(async_io::descriptor_view descriptor,
                                          mutable_buffer buffer,
                                          std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::read_model(descriptor, buffer, offset),
      submit_mode::direct);
}

inline auto io_context::async_read_some_direct(
    async_io::descriptor_view descriptor, mutable_buffer buffer,
    std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::read_model(descriptor, buffer, offset),
      submit_mode::direct);
}

inline auto io_context::async_write(async_io::descriptor_view descriptor,
                                    const_buffer buffer, std::uint64_t offset) {
  return detail::write_all_sender(detail::descriptor_write_all_state(
      *this, descriptor, buffer, offset, submit_mode::queued));
}

inline auto io_context::async_write_some(async_io::descriptor_view descriptor,
                                         const_buffer buffer,
                                         std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::write_model(descriptor, buffer, offset),
      submit_mode::queued);
}

inline auto io_context::async_write_direct(async_io::descriptor_view descriptor,
                                           const_buffer buffer,
                                           std::uint64_t offset) {
  return detail::write_all_sender(detail::descriptor_write_all_state(
      *this, descriptor, buffer, offset, submit_mode::direct));
}

inline auto io_context::async_write_some_direct(
    async_io::descriptor_view descriptor, const_buffer buffer,
    std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::write_model(descriptor, buffer, offset),
      submit_mode::direct);
}

inline auto io_context::async_accept(async_io::listening_socket_view socket,
                                     int flags) {
  return detail::native_io_sender(*this, detail::accept_model(socket, flags),
                                  submit_mode::queued);
}

inline auto io_context::async_accept_direct(
    async_io::listening_socket_view socket, int flags) {
  return detail::native_io_sender(*this, detail::accept_model(socket, flags),
                                  submit_mode::direct);
}

inline auto io_context::async_connect(async_io::stream_socket_view socket,
                                      const ip::endpoint& endpoint) {
  return detail::native_io_sender(
      *this, detail::connect_model(socket, endpoint), submit_mode::queued);
}

inline auto io_context::async_connect_direct(
    async_io::stream_socket_view socket, const ip::endpoint& endpoint) {
  return detail::native_io_sender(
      *this, detail::connect_model(socket, endpoint), submit_mode::direct);
}

inline auto io_context::async_poll(async_io::descriptor_view descriptor,
                                   unsigned poll_mask) {
  return detail::native_io_sender(
      *this, detail::poll_model(descriptor, poll_mask), submit_mode::queued);
}

inline auto io_context::async_poll_direct(async_io::descriptor_view descriptor,
                                          unsigned poll_mask) {
  return detail::native_io_sender(
      *this, detail::poll_model(descriptor, poll_mask), submit_mode::direct);
}

inline auto io_context::async_resolve(async_io::dns_query query,
                                      async_io::dns_result_view result) {
  return native_context_.async_resolve(std::move(query), result);
}

inline auto io_context::async_resolve(std::string_view host,
                                      std::string_view service,
                                      async_io::dns_result_view result) {
  return async_resolve(async_io::dns_query(host, service), result);
}

inline auto steady_timer::async_wait() {
  return detail::timer_wait_sender(*this);
}

/** @endcond */

}  // namespace bupp

#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_H_
