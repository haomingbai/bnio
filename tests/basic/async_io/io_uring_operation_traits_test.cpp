#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "../../support/async_io/io_uring_context_test_support.h"

namespace {

using namespace bnio_async_io_io_uring_test;

struct queue_test_operation : io_uring_operation_base {
  void execute() noexcept override {}
};

struct io_queue_test_operation
    : bnio::async_io::linux_native::io_uring_io_operation_base {
  void prepare(bnio::base::submission_queue_entry&) noexcept override {}
  void complete_submit_error(int) noexcept override {}
  void execute() noexcept override {}
};

TEST(IoUringOperationTraitsTest, shared_task_queue_separates_cpu_and_io) {
  bnio::async_io::linux_native::io_uring_task_queue_state queue;
  queue_test_operation cpu;
  std::array<io_queue_test_operation, 3> io;

  for (io_queue_test_operation& operation : io) {
    queue.push_io(operation);
  }
  queue.push_cpu(cpu);

  EXPECT_EQ(queue.pop_cpu_all(), &cpu);
  EXPECT_EQ(queue.pop_cpu_all(), nullptr);

  std::size_t io_count = 0;
  for (auto* operation = queue.pop_io_all(); operation != nullptr;
       operation = static_cast<io_queue_test_operation*>(operation->next)) {
    ++io_count;
  }
  EXPECT_EQ(io_count, io.size());
  EXPECT_EQ(queue.pop_io_all(), nullptr);
  EXPECT_FALSE(queue.closing.load(std::memory_order_acquire));
}

TEST(IoUringOperationTraitsTest, operation_state_concepts) {
  static_assert(bexec::operation_state<io_uring_post_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_nop_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_timeout_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_poll_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_accept_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_connect_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_recv_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_send_operation<receiver>>);
  static_assert(
      bexec::operation_state<io_uring_datagram_receive_operation<receiver>>);
  static_assert(
      bexec::operation_state<io_uring_datagram_send_operation<receiver>>);
  static_assert(
      bexec::operation_state<io_uring_receive_from_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_send_to_operation<receiver>>);
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
          bnio::async_io::dns_query{}, bnio::async_io::dns_result_view{}));
  using poll_sender = decltype(std::declval<io_uring_context&>().async_poll(
      descriptor_view{}, static_cast<unsigned>(POLLIN)));
  static_assert(bexec::sender<poll_sender>);
  static_assert(bexec::sender<resolve_sender>);

  static_assert(
      !std::is_constructible_v<io_uring_accept_operation<receiver>,
                               io_uring_context&, datagram_socket_view,
                               bnio::async_io::ip::endpoint&, int, receiver>);
  static_assert(
      !std::is_constructible_v<io_uring_connect_operation<receiver>,
                               io_uring_context&, datagram_socket_view,
                               const bnio::async_io::ip::endpoint&, receiver>);
  static_assert(
      !std::is_constructible_v<io_uring_recv_operation<receiver>,
                               io_uring_context&, datagram_socket_view,
                               const buffer_view&, int, receiver>);
  static_assert(
      !std::is_constructible_v<io_uring_send_operation<receiver>,
                               io_uring_context&, datagram_socket_view,
                               const buffer_view&, int, receiver>);
  static_assert(
      !std::is_constructible_v<io_uring_datagram_receive_operation<receiver>,
                               io_uring_context&, stream_socket_view,
                               const buffer_view&, int, receiver>);
  static_assert(
      !std::is_constructible_v<io_uring_datagram_send_operation<receiver>,
                               io_uring_context&, stream_socket_view,
                               const buffer_view&, int, receiver>);
}

TEST(IoUringOperationTraitsTest, io_operations_accept_async_io_views) {
  io_uring_context context;
  stream_socket_view listener(3);
  stream_socket_view stream(4);
  datagram_socket_view datagram(5);
  descriptor_view descriptor(4);

  char data[16]{};
  buffer_view buffer{data, sizeof(data)};
  bnio::async_io::ip::endpoint remote_endpoint =
      bnio::async_io::ip::endpoint::loopback_v4(80);
  msghdr message{};
  mutable_message_view receive_message(message);
  const_message_view send_message(message);
  iovec vector{data, sizeof(data)};
  buffer_sequence_view vectors(&vector, 1);
  bnio::async_io::duration timeout{};

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
  [[maybe_unused]] io_uring_datagram_receive_operation
      datagram_receive_operation(context, datagram, buffer, 0, receiver{});
  [[maybe_unused]] io_uring_datagram_send_operation datagram_send_operation(
      context, datagram, buffer, 0, receiver{});
  [[maybe_unused]] io_uring_receive_from_operation receive_from_operation(
      context, datagram, buffer, remote_endpoint, 0, receiver{});
  [[maybe_unused]] io_uring_send_to_operation send_to_operation(
      context, datagram, buffer, remote_endpoint, 0, receiver{});
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
  bnio::async_io::ip::endpoint resolved_endpoints[4]{};
  [[maybe_unused]] io_uring_resolve_operation resolve_operation(
      context, bnio::async_io::dns_query("127.0.0.1", "80"),
      bnio::async_io::dns_result_view(resolved_endpoints), resolve_receiver{});
}

TEST(IoUringOperationTraitsTest, timeout_operation_prepares_async_io_time) {
  io_uring_context context;
  io_uring_sqe raw_sqe{};
  bnio::base::submission_queue_entry sqe(&raw_sqe);

  io_uring_timeout_operation operation(
      context, std::chrono::seconds(2) + std::chrono::nanoseconds(5), 3,
      IORING_TIMEOUT_ABS, receiver{});
  operation.prepare(sqe);

  const auto* timeout = reinterpret_cast<const __kernel_timespec*>(
      static_cast<std::uintptr_t>(raw_sqe.addr));
  EXPECT_EQ(raw_sqe.opcode, IORING_OP_TIMEOUT);
  EXPECT_EQ(raw_sqe.timeout_flags, IORING_TIMEOUT_ABS);
  EXPECT_NE(timeout, nullptr);
  EXPECT_EQ(timeout->tv_sec, 2);
  EXPECT_EQ(timeout->tv_nsec, 5);
}

}  // namespace
