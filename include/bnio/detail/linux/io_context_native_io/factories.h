/**
 * @file factories.h
 * @brief Linux native I/O sender factories.
 */

#ifndef BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_FACTORIES_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_FACTORIES_H_

#include <bnio/detail/linux/io_context_native_io/common.h>
#include <bnio/detail/linux/io_context_native_io/file.h>
#include <bnio/detail/linux/io_context_native_io/poll.h>
#include <bnio/detail/linux/io_context_native_io/socket.h>

namespace bnio::detail {

[[nodiscard]] inline auto make_stream_read_request(
    async_io::stream_socket_view socket, mutable_buffer buffer, int flags) {
  return socket_read_model(socket, buffer, flags);
}

[[nodiscard]] inline auto make_stream_write_request(
    async_io::stream_socket_view socket, const_buffer buffer, int flags) {
  return socket_write_model(socket, buffer, flags);
}

[[nodiscard]] inline auto make_datagram_receive_request(
    async_io::datagram_socket_view socket, mutable_buffer buffer, int flags) {
  return datagram_receive_model(socket, buffer, flags);
}

[[nodiscard]] inline auto make_datagram_send_request(
    async_io::datagram_socket_view socket, const_buffer buffer, int flags) {
  return datagram_send_model(socket, buffer, flags);
}

[[nodiscard]] inline auto make_datagram_receive_from_request(
    async_io::datagram_socket_view socket, mutable_buffer buffer,
    ip::endpoint& endpoint, int flags) {
  return datagram_receive_from_model(socket, buffer, endpoint, flags);
}

[[nodiscard]] inline auto make_datagram_send_to_request(
    async_io::datagram_socket_view socket, const_buffer buffer,
    const ip::endpoint& endpoint, int flags) {
  return datagram_send_to_model(socket, buffer, endpoint, flags);
}

[[nodiscard]] inline auto make_file_read_request(
    async_io::descriptor_view descriptor, mutable_buffer buffer,
    std::uint64_t offset) {
  return read_model(descriptor, buffer, offset);
}

[[nodiscard]] inline auto make_file_write_request(
    async_io::descriptor_view descriptor, const_buffer buffer,
    std::uint64_t offset) {
  return write_model(descriptor, buffer, offset);
}

[[nodiscard]] inline auto make_accept_request(
    async_io::stream_socket_view socket, int flags) {
  return accept_model(socket, flags);
}

[[nodiscard]] inline auto make_connect_request(
    async_io::stream_socket_view socket, const ip::endpoint& endpoint) {
  return connect_model(socket, endpoint);
}

[[nodiscard]] inline auto make_poll_sender(io_context& context,
                                           async_io::descriptor_view descriptor,
                                           unsigned poll_mask) {
  return native_io_sender(context, poll_model(descriptor, poll_mask));
}

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_FACTORIES_H_
