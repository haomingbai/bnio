#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_H_
#ifndef BUPP_LINUX_IO_CONTEXT_H_
#include <bupp/linux/io_context.h>
#else
#define BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_H_

#include <bupp/linux/detail/io_context_native_io/common.h>
#include <bupp/linux/detail/io_context_native_io/file.h>
#include <bupp/linux/detail/io_context_native_io/poll.h>
#include <bupp/linux/detail/io_context_native_io/socket.h>
#include <bupp/linux/detail/io_context_native_io/timer_wait.h>
#include <bupp/linux/detail/io_context_native_io/write_all.h>

namespace bupp {

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

inline auto io_context::async_receive(async_io::datagram_socket_view socket,
                                      mutable_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::datagram_receive_model(socket, buffer, flags),
      submit_mode::queued);
}

inline auto io_context::async_receive_direct(
    async_io::datagram_socket_view socket, mutable_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::datagram_receive_model(socket, buffer, flags),
      submit_mode::direct);
}

inline auto io_context::async_send(async_io::datagram_socket_view socket,
                                   const_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::datagram_send_model(socket, buffer, flags),
      submit_mode::queued);
}

inline auto io_context::async_send_direct(async_io::datagram_socket_view socket,
                                          const_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::datagram_send_model(socket, buffer, flags),
      submit_mode::direct);
}

inline auto io_context::async_receive_from(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    ip::endpoint& endpoint, int flags) {
  return detail::native_io_sender(
      *this,
      detail::datagram_receive_from_model(socket, buffer, endpoint, flags),
      submit_mode::queued);
}

inline auto io_context::async_receive_from_direct(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    ip::endpoint& endpoint, int flags) {
  return detail::native_io_sender(
      *this,
      detail::datagram_receive_from_model(socket, buffer, endpoint, flags),
      submit_mode::direct);
}

inline auto io_context::async_send_to(async_io::datagram_socket_view socket,
                                      const_buffer buffer,
                                      const ip::endpoint& endpoint, int flags) {
  return detail::native_io_sender(
      *this, detail::datagram_send_to_model(socket, buffer, endpoint, flags),
      submit_mode::queued);
}

inline auto io_context::async_send_to_direct(
    async_io::datagram_socket_view socket, const_buffer buffer,
    const ip::endpoint& endpoint, int flags) {
  return detail::native_io_sender(
      *this, detail::datagram_send_to_model(socket, buffer, endpoint, flags),
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

inline auto io_context::async_accept(async_io::stream_socket_view socket,
                                     int flags) {
  return detail::native_io_sender(*this, detail::accept_model(socket, flags),
                                  submit_mode::queued);
}

inline auto io_context::async_accept_direct(async_io::stream_socket_view socket,
                                            int flags) {
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
auto io_context::basic_scheduler<Kind>::async_read_some(
    async_io::stream_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_read_some(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read_direct(
    async_io::stream_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_read_direct(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read_some_direct(
    async_io::stream_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_read_some_direct(socket, buffer, flags);
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
auto io_context::basic_scheduler<Kind>::async_write_direct(
    async_io::stream_socket_view socket, const_buffer buffer, int flags) const {
  return context_->async_write_direct(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write_some_direct(
    async_io::stream_socket_view socket, const_buffer buffer, int flags) const {
  return context_->async_write_some_direct(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_receive(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_receive(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_receive_direct(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    int flags) const {
  return context_->async_receive_direct(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_send(
    async_io::datagram_socket_view socket, const_buffer buffer,
    int flags) const {
  return context_->async_send(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_send_direct(
    async_io::datagram_socket_view socket, const_buffer buffer,
    int flags) const {
  return context_->async_send_direct(socket, buffer, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_receive_from(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    ip::endpoint& endpoint, int flags) const {
  return context_->async_receive_from(socket, buffer, endpoint, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_receive_from_direct(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    ip::endpoint& endpoint, int flags) const {
  return context_->async_receive_from_direct(socket, buffer, endpoint, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_send_to(
    async_io::datagram_socket_view socket, const_buffer buffer,
    const ip::endpoint& endpoint, int flags) const {
  return context_->async_send_to(socket, buffer, endpoint, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_send_to_direct(
    async_io::datagram_socket_view socket, const_buffer buffer,
    const ip::endpoint& endpoint, int flags) const {
  return context_->async_send_to_direct(socket, buffer, endpoint, flags);
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
auto io_context::basic_scheduler<Kind>::async_read_direct(
    async_io::descriptor_view descriptor, mutable_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_read_direct(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read_some_direct(
    async_io::descriptor_view descriptor, mutable_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_read_some_direct(descriptor, buffer, offset);
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
auto io_context::basic_scheduler<Kind>::async_write_direct(
    async_io::descriptor_view descriptor, const_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_write_direct(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write_some_direct(
    async_io::descriptor_view descriptor, const_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_write_some_direct(descriptor, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_accept(
    async_io::stream_socket_view socket, int flags) const {
  return context_->async_accept(socket, flags);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_accept_direct(
    async_io::stream_socket_view socket, int flags) const {
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

}  // namespace bupp

#endif  // BUPP_LINUX_IO_CONTEXT_H_
#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_H_
