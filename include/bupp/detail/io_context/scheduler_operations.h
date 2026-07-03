#pragma once
#ifndef BUPP_DETAIL_IO_CONTEXT_SCHEDULER_OPERATIONS_H_
#define BUPP_DETAIL_IO_CONTEXT_SCHEDULER_OPERATIONS_H_

#include <bupp/linux/io_context.h>

#include <utility>

namespace bupp {

/** @cond BUPP_DETAIL */

template <io_context::schedule_kind Kind>
std::error_code io_context::basic_scheduler<Kind>::flush_io_queue()
    const noexcept {
  return context_->flush_io_queue();
}

template <io_context::schedule_kind Kind>
std::size_t io_context::basic_scheduler<Kind>::queued_io_size() const noexcept {
  return context_->queued_io_size();
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read(
    async_io::stream_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_read(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read_direct(
    async_io::stream_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_read_direct(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write(
    async_io::stream_socket_view socket, const_buffer buffer, int flags) const {
  return context_->async_write(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write_direct(
    async_io::stream_socket_view socket, const_buffer buffer, int flags) const {
  return context_->async_write_direct(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read(
    async_io::descriptor_view descriptor, mutable_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_read(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read_direct(
    async_io::descriptor_view descriptor, mutable_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_read_direct(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write(
    async_io::descriptor_view descriptor, const_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_write(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write_direct(
    async_io::descriptor_view descriptor, const_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_write_direct(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_accept(
    async_io::listening_socket_view socket, int flags) const {
  return context_->async_accept(socket, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_accept_direct(
    async_io::listening_socket_view socket, int flags) const {
  return context_->async_accept_direct(socket, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_connect(
    async_io::stream_socket_view socket, const ip::endpoint& endpoint) const {
  return context_->async_connect(socket, endpoint);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_connect_direct(
    async_io::stream_socket_view socket, const ip::endpoint& endpoint) const {
  return context_->async_connect_direct(socket, endpoint);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_poll(
    async_io::descriptor_view descriptor, unsigned poll_mask) const {
  return context_->async_poll(descriptor, poll_mask);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_poll_direct(
    async_io::descriptor_view descriptor, unsigned poll_mask) const {
  return context_->async_poll_direct(descriptor, poll_mask);
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

/** @endcond */

}  // namespace bupp

#endif  // BUPP_DETAIL_IO_CONTEXT_SCHEDULER_OPERATIONS_H_
