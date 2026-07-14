#ifndef BUPP_BSD_DETAIL_IO_CONTEXT_NATIVE_IO_H_
#ifndef BUPP_BSD_IO_CONTEXT_H_
#include <bupp/bsd/io_context.h>
#else
#define BUPP_BSD_DETAIL_IO_CONTEXT_NATIVE_IO_H_

#include <bupp/bsd/detail/io_context_native_io/common.h>
#include <bupp/bsd/detail/io_context_native_io/timer_wait.h>
#include <bupp/bsd/detail/io_context_native_io/write_all.h>

namespace bupp {

inline auto io_context::async_read(async_io::stream_socket_view socket,
                                   mutable_buffer buffer, int flags) {
  return select_native_context().async_receive(socket, buffer.view(), flags);
}

inline auto io_context::async_read_some(async_io::stream_socket_view socket,
                                        mutable_buffer buffer, int flags) {
  return select_native_context().async_receive(socket, buffer.view(), flags);
}

inline auto io_context::async_write(async_io::stream_socket_view socket,
                                    const_buffer buffer, int flags) {
  return detail::write_all_sender(
      detail::socket_write_all_state(*this, socket, buffer, flags));
}

inline auto io_context::async_write_some(async_io::stream_socket_view socket,
                                         const_buffer buffer, int flags) {
  return select_native_context().async_send(socket, buffer.data(),
                                            buffer.size(), flags);
}

inline auto io_context::async_receive(async_io::datagram_socket_view socket,
                                      mutable_buffer buffer, int flags) {
  return select_native_context().async_receive(socket, buffer.view(), flags);
}

inline auto io_context::async_send(async_io::datagram_socket_view socket,
                                   const_buffer buffer, int flags) {
  return select_native_context().async_send(socket, buffer.data(),
                                            buffer.size(), flags);
}

inline auto io_context::async_receive_from(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    ip::endpoint& endpoint, int flags) {
  return select_native_context().async_receive_from(socket, buffer.view(),
                                                    endpoint, flags);
}

inline auto io_context::async_send_to(async_io::datagram_socket_view socket,
                                      const_buffer buffer,
                                      const ip::endpoint& endpoint, int flags) {
  return select_native_context().async_send_to(socket, buffer.data(),
                                               buffer.size(), endpoint, flags);
}

inline auto io_context::async_read(async_io::descriptor_view descriptor,
                                   mutable_buffer buffer,
                                   std::uint64_t offset) {
  return select_native_context().async_read(descriptor, buffer.view(), offset);
}

inline auto io_context::async_read_some(async_io::descriptor_view descriptor,
                                        mutable_buffer buffer,
                                        std::uint64_t offset) {
  return select_native_context().async_read(descriptor, buffer.view(), offset);
}

inline auto io_context::async_write(async_io::descriptor_view descriptor,
                                    const_buffer buffer, std::uint64_t offset) {
  return detail::write_all_sender(
      detail::descriptor_write_all_state(*this, descriptor, buffer, offset));
}

inline auto io_context::async_write_some(async_io::descriptor_view descriptor,
                                         const_buffer buffer,
                                         std::uint64_t offset) {
  return select_native_context().async_write(descriptor, buffer.data(),
                                             buffer.size(), offset);
}

inline auto io_context::async_accept(async_io::stream_socket_view socket,
                                     int flags) {
  return select_native_context().async_accept(socket, flags);
}

inline auto io_context::async_connect(async_io::stream_socket_view socket,
                                      const ip::endpoint& endpoint) {
  return select_native_context().async_connect(socket, endpoint);
}

inline auto io_context::async_poll(async_io::descriptor_view descriptor,
                                   unsigned poll_mask) {
  return select_native_context().async_poll(descriptor, poll_mask);
}

inline auto io_context::async_resolve(async_io::dns_query query,
                                      async_io::dns_result_view result) {
  return select_native_context().async_resolve(std::move(query), result);
}

inline auto io_context::async_resolve(std::string_view host,
                                      std::string_view service,
                                      async_io::dns_result_view result) {
  return async_resolve(async_io::dns_query(host, service), result);
}

inline auto steady_timer::async_wait() {
  return detail::timer_wait_sender(*this);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read(
    async_io::stream_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_read(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read_some(
    async_io::stream_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_read_some(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write(
    async_io::stream_socket_view socket, const_buffer buffer, int flags) const {
  return context_->async_write(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write_some(
    async_io::stream_socket_view socket, const_buffer buffer, int flags) const {
  return context_->async_write_some(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_receive(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_receive(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_send(
    async_io::datagram_socket_view socket, const_buffer buffer,
    int flags) const {
  return context_->async_send(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_receive_from(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    ip::endpoint& endpoint, int flags) const {
  return context_->async_receive_from(socket, buffer, endpoint, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_send_to(
    async_io::datagram_socket_view socket, const_buffer buffer,
    const ip::endpoint& endpoint, int flags) const {
  return context_->async_send_to(socket, buffer, endpoint, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read(
    async_io::descriptor_view descriptor, mutable_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_read(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read_some(
    async_io::descriptor_view descriptor, mutable_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_read_some(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write(
    async_io::descriptor_view descriptor, const_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_write(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write_some(
    async_io::descriptor_view descriptor, const_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_write_some(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_accept(
    async_io::stream_socket_view socket, int flags) const {
  return context_->async_accept(socket, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_connect(
    async_io::stream_socket_view socket, const ip::endpoint& endpoint) const {
  return context_->async_connect(socket, endpoint);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_poll(
    async_io::descriptor_view descriptor, unsigned poll_mask) const {
  return context_->async_poll(descriptor, poll_mask);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_resolve(
    async_io::dns_query query, async_io::dns_result_view result) const {
  return context_->async_resolve(std::move(query), result);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_resolve(
    std::string_view host, std::string_view service,
    async_io::dns_result_view result) const {
  return context_->async_resolve(host, service, result);
}

}  // namespace bupp

#endif  // BUPP_BSD_IO_CONTEXT_H_
#endif  // BUPP_BSD_DETAIL_IO_CONTEXT_NATIVE_IO_H_
