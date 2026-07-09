#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "io_uring_context_test_support.h"

namespace {

using namespace bupp_async_io_io_uring_test;

void test_operation_state_concepts() {
  static_assert(bexec::operation_state<io_uring_post_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_nop_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_timeout_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_poll_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_accept_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_connect_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_recv_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_send_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_recvmsg_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_sendmsg_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_read_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_write_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_readv_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_writev_operation<receiver>>);
  static_assert(
      bexec::operation_state<io_uring_resolve_operation<resolve_receiver>>);

  using resolve_sender =
      decltype(std::declval<io_uring_context&>().async_resolve(
          bupp::async_io::dns_query{}, bupp::async_io::dns_result_view{}));
  using poll_sender = decltype(std::declval<io_uring_context&>().async_poll(
      descriptor_view{}, static_cast<unsigned>(POLLIN)));
  static_assert(bexec::sender<poll_sender>);
  static_assert(bexec::sender<resolve_sender>);

  static_assert(
      !std::is_constructible_v<io_uring_accept_operation<receiver>,
                               io_uring_context&, stream_socket_view,
                               bupp::async_io::ip::endpoint&, int, receiver>);
  static_assert(
      !std::is_constructible_v<io_uring_connect_operation<receiver>,
                               io_uring_context&, listening_socket_view,
                               const bupp::async_io::ip::endpoint&, receiver>);
  static_assert(
      !std::is_constructible_v<io_uring_recv_operation<receiver>,
                               io_uring_context&, listening_socket_view,
                               const buffer_view&, int, receiver>);
  static_assert(
      !std::is_constructible_v<io_uring_send_operation<receiver>,
                               io_uring_context&, listening_socket_view,
                               const buffer_view&, int, receiver>);
}

void test_io_operations_accept_async_io_views() {
  io_uring_context context;
  listening_socket_view listener(3);
  stream_socket_view stream(4);
  descriptor_view descriptor(4);

  char data[16]{};
  buffer_view buffer{data, sizeof(data)};
  bupp::async_io::ip::endpoint remote_endpoint =
      bupp::async_io::ip::endpoint::loopback_v4(80);
  msghdr message{};
  mutable_message_view receive_message(message);
  const_message_view send_message(message);
  iovec vector{data, sizeof(data)};
  buffer_sequence_view vectors(&vector, 1);
  bupp::async_io::duration timeout{};

  [[maybe_unused]] io_uring_timeout_operation timeout_operation(
      context, timeout, 0, 0, receiver{});
  [[maybe_unused]] io_uring_timeout_operation chrono_timeout_operation(
      context, std::chrono::milliseconds(1), 0, 0, receiver{});
  [[maybe_unused]] io_uring_poll_operation poll_operation(context, descriptor,
                                                          0, receiver{});
  [[maybe_unused]] io_uring_accept_operation accept_operation(
      context, listener, remote_endpoint, 0, receiver{});
  [[maybe_unused]] io_uring_accept_operation accept_without_endpoint_operation(
      context, listener, 0, receiver{});
  [[maybe_unused]] io_uring_connect_operation connect_operation(
      context, stream, remote_endpoint, receiver{});
  [[maybe_unused]] io_uring_recv_operation recv_operation(
      context, stream, buffer, 0, receiver{});
  [[maybe_unused]] io_uring_send_operation send_operation(
      context, stream, buffer, 0, receiver{});
  [[maybe_unused]] io_uring_recvmsg_operation recvmsg_operation(
      context, stream, receive_message, 0, receiver{});
  [[maybe_unused]] io_uring_sendmsg_operation sendmsg_operation(
      context, stream, send_message, 0, receiver{});
  [[maybe_unused]] io_uring_read_operation read_operation(
      context, descriptor, buffer, 0, receiver{});
  [[maybe_unused]] io_uring_write_operation write_operation(
      context, descriptor, buffer, 0, receiver{});
  [[maybe_unused]] io_uring_readv_operation readv_operation(
      context, descriptor, vectors, 0, receiver{});
  [[maybe_unused]] io_uring_writev_operation writev_operation(
      context, descriptor, vectors, 0, receiver{});
  bupp::async_io::ip::endpoint resolved_endpoints[4]{};
  [[maybe_unused]] io_uring_resolve_operation resolve_operation(
      context, bupp::async_io::dns_query("127.0.0.1", "80"),
      bupp::async_io::dns_result_view(resolved_endpoints), resolve_receiver{});
}

void test_timeout_operation_prepares_async_io_time() {
  io_uring_context context;
  io_uring_sqe raw_sqe{};
  bupp::base::submission_queue_entry sqe(&raw_sqe);

  io_uring_timeout_operation operation(
      context, std::chrono::seconds(2) + std::chrono::nanoseconds(5), 3,
      IORING_TIMEOUT_ABS, receiver{});
  operation.prepare(sqe);

  const auto* timeout = reinterpret_cast<const __kernel_timespec*>(
      static_cast<std::uintptr_t>(raw_sqe.addr));
  assert(raw_sqe.opcode == IORING_OP_TIMEOUT);
  assert(raw_sqe.timeout_flags == IORING_TIMEOUT_ABS);
  assert(timeout != nullptr);
  assert(timeout->tv_sec == 2);
  assert(timeout->tv_nsec == 5);
}

}  // namespace

int main() {
  test_operation_state_concepts();
  test_io_operations_accept_async_io_views();
  test_timeout_operation_prepares_async_io_time();
  return 0;
}
