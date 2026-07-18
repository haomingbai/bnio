#include <gtest/gtest.h>

#include "../../support/io_context/io_context_test_support.h"

namespace {

TEST(IoContextSenderConceptTest, sender_concepts) {
  bnio::io_context context;
  auto scheduler = context.get_post_scheduler();
  bnio::tcp_socket socket(3);
  std::array<char, 8> bytes{};
  constexpr std::string_view text = "abc";

  using stream_read_sender =
      decltype(socket.async_read(scheduler, bnio::buffer(bytes)));
  using stream_read_some_sender =
      decltype(socket.async_read_some(scheduler, bnio::buffer(bytes)));
  using low_read_sender =
      decltype(scheduler.async_read(socket.view(), bnio::buffer(bytes)));
  using low_read_some_sender =
      decltype(scheduler.async_read_some(socket.view(), bnio::buffer(bytes)));
  using low_write_sender =
      decltype(scheduler.async_write(socket.view(), bnio::buffer(text)));
  using low_write_some_sender =
      decltype(scheduler.async_write_some(socket.view(), bnio::buffer(text)));
  using stream_write_sender = decltype(socket.async_write(scheduler, text));
  using stream_write_some_sender =
      decltype(socket.async_write_some(scheduler, text));
  using accept_sender =
      decltype(std::declval<bnio::tcp_acceptor&>().async_accept(scheduler));
  using connect_sender =
      decltype(std::declval<bnio::tcp_socket&>().async_connect(
          scheduler, std::declval<const bnio::ip::endpoint&>()));
  using descriptor_read_sender = decltype(scheduler.async_read(
      bnio::async_io::descriptor_view(3), bnio::buffer(bytes)));
  using descriptor_read_some_sender = decltype(scheduler.async_read_some(
      bnio::async_io::descriptor_view(3), bnio::buffer(bytes)));
  using descriptor_write_sender = decltype(scheduler.async_write(
      bnio::async_io::descriptor_view(3), bnio::buffer(text)));
  using descriptor_write_some_sender = decltype(scheduler.async_write_some(
      bnio::async_io::descriptor_view(3), bnio::buffer(text)));
  using poll_sender = decltype(scheduler.async_poll(
      bnio::async_io::descriptor_view(3), static_cast<unsigned>(POLLIN)));
  using schedule_sender = decltype(bexec::schedule(scheduler));
  using timer_wait_sender =
      decltype(std::declval<bnio::steady_timer&>().async_wait());
  static_assert(bexec::sender<stream_read_sender>);
  static_assert(bexec::sender<stream_read_some_sender>);
  static_assert(bexec::sender<low_read_sender>);
  static_assert(bexec::sender<low_read_some_sender>);
  static_assert(bexec::sender<low_write_sender>);
  static_assert(bexec::sender<low_write_some_sender>);
  static_assert(bexec::sender<stream_write_sender>);
  static_assert(bexec::sender<stream_write_some_sender>);
  static_assert(bexec::sender<accept_sender>);
  static_assert(bexec::sender<connect_sender>);
  static_assert(bexec::sender<descriptor_read_sender>);
  static_assert(bexec::sender<descriptor_read_some_sender>);
  static_assert(bexec::sender<descriptor_write_sender>);
  static_assert(bexec::sender<descriptor_write_some_sender>);
  static_assert(bexec::sender<poll_sender>);
  static_assert(bexec::sender<schedule_sender>);
  static_assert(bexec::sender<timer_wait_sender>);
  static_assert(bexec::scheduler<bnio::io_context::dispatch_scheduler>);
  static_assert(bexec::scheduler<bnio::io_context::post_scheduler>);
  static_assert(
      !scheduler_can_read_stream<bnio::io_context::post_scheduler,
                                 bnio::tcp_socket, bnio::mutable_buffer>);
  static_assert(
      !scheduler_can_write_stream<bnio::io_context::post_scheduler,
                                  bnio::tcp_socket, bnio::const_buffer>);
  static_assert(bnio::reads_bytes<bnio::io_context::post_scheduler,
                                  bnio::tcp_socket, bnio::mutable_buffer>);
  static_assert(bnio::reads_bytes<bnio::io_context::dispatch_scheduler,
                                  bnio::tcp_socket, bnio::mutable_buffer>);
  static_assert(bnio::writes_bytes<bnio::io_context::post_scheduler,
                                   bnio::tcp_socket, bnio::const_buffer>);
  static_assert(bnio::writes_bytes<bnio::io_context::dispatch_scheduler,
                                   bnio::tcp_socket, bnio::const_buffer>);
  static_assert(bnio::accepts_connections<bnio::io_context::post_scheduler,
                                          bnio::tcp_acceptor>);
  static_assert(
      bnio::connects_stream<bnio::io_context::post_scheduler, bnio::tcp_socket,
                            const bnio::ip::endpoint&>);
  static_assert(
      bnio::reads_bytes<bnio::io_context::post_scheduler,
                        bnio::async_io::descriptor_view, bnio::mutable_buffer>);
  static_assert(
      bnio::writes_bytes<bnio::io_context::post_scheduler,
                         bnio::async_io::descriptor_view, bnio::const_buffer>);
  static_assert(bnio::polls_descriptor<bnio::io_context::post_scheduler,
                                       bnio::async_io::descriptor_view>);

  byte_receiver receiver;
  auto sender = socket.async_read(scheduler, bnio::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  static_assert(bexec::operation_state<decltype(operation)>);

  (void)socket.release();
}

}  // namespace
