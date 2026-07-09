#include "io_context_test_support.h"

namespace {

void test_sender_concepts() {
  bupp::io_context context;
  auto scheduler = context.get_post_scheduler();
  bupp::tcp_socket socket(3);
  std::array<char, 8> bytes{};
  constexpr std::string_view text = "abc";

  using stream_read_sender =
      decltype(socket.async_read(scheduler, bupp::buffer(bytes)));
  using stream_read_some_sender =
      decltype(socket.async_read_some(scheduler, bupp::buffer(bytes)));
  using stream_read_direct_sender =
      decltype(socket.async_read_direct(scheduler, bupp::buffer(bytes)));
  using stream_read_some_direct_sender =
      decltype(socket.async_read_some_direct(scheduler, bupp::buffer(bytes)));
  using low_read_sender =
      decltype(scheduler.async_read(socket.view(), bupp::buffer(bytes)));
  using low_read_some_sender =
      decltype(scheduler.async_read_some(socket.view(), bupp::buffer(bytes)));
  using low_write_sender =
      decltype(scheduler.async_write(socket.view(), bupp::buffer(text)));
  using low_write_some_sender =
      decltype(scheduler.async_write_some(socket.view(), bupp::buffer(text)));
  using stream_write_sender = decltype(socket.async_write(scheduler, text));
  using stream_write_some_sender =
      decltype(socket.async_write_some(scheduler, text));
  using stream_write_direct_sender =
      decltype(socket.async_write_direct(scheduler, text));
  using stream_write_some_direct_sender =
      decltype(socket.async_write_some_direct(scheduler, text));
  using accept_sender =
      decltype(std::declval<bupp::tcp_acceptor&>().async_accept(scheduler));
  using accept_direct_sender =
      decltype(std::declval<bupp::tcp_acceptor&>().async_accept_direct(
          scheduler));
  using connect_sender =
      decltype(std::declval<bupp::tcp_socket&>().async_connect(
          scheduler, std::declval<const bupp::ip::endpoint&>()));
  using connect_direct_sender =
      decltype(std::declval<bupp::tcp_socket&>().async_connect_direct(
          scheduler, std::declval<const bupp::ip::endpoint&>()));
  using descriptor_read_sender = decltype(scheduler.async_read(
      bupp::async_io::descriptor_view(3), bupp::buffer(bytes)));
  using descriptor_read_some_sender = decltype(scheduler.async_read_some(
      bupp::async_io::descriptor_view(3), bupp::buffer(bytes)));
  using descriptor_read_direct_sender = decltype(scheduler.async_read_direct(
      bupp::async_io::descriptor_view(3), bupp::buffer(bytes)));
  using descriptor_read_some_direct_sender =
      decltype(scheduler.async_read_some_direct(
          bupp::async_io::descriptor_view(3), bupp::buffer(bytes)));
  using descriptor_write_sender = decltype(scheduler.async_write(
      bupp::async_io::descriptor_view(3), bupp::buffer(text)));
  using descriptor_write_some_sender = decltype(scheduler.async_write_some(
      bupp::async_io::descriptor_view(3), bupp::buffer(text)));
  using descriptor_write_direct_sender = decltype(scheduler.async_write_direct(
      bupp::async_io::descriptor_view(3), bupp::buffer(text)));
  using descriptor_write_some_direct_sender =
      decltype(scheduler.async_write_some_direct(
          bupp::async_io::descriptor_view(3), bupp::buffer(text)));
  using poll_sender = decltype(scheduler.async_poll(
      bupp::async_io::descriptor_view(3), static_cast<unsigned>(POLLIN)));
  using schedule_sender = decltype(bexec::schedule(scheduler));
  using timer_wait_sender =
      decltype(std::declval<bupp::steady_timer&>().async_wait());
  static_assert(bexec::sender<stream_read_sender>);
  static_assert(bexec::sender<stream_read_some_sender>);
  static_assert(bexec::sender<stream_read_direct_sender>);
  static_assert(bexec::sender<stream_read_some_direct_sender>);
  static_assert(bexec::sender<low_read_sender>);
  static_assert(bexec::sender<low_read_some_sender>);
  static_assert(bexec::sender<low_write_sender>);
  static_assert(bexec::sender<low_write_some_sender>);
  static_assert(bexec::sender<stream_write_sender>);
  static_assert(bexec::sender<stream_write_some_sender>);
  static_assert(bexec::sender<stream_write_direct_sender>);
  static_assert(bexec::sender<stream_write_some_direct_sender>);
  static_assert(bexec::sender<accept_sender>);
  static_assert(bexec::sender<accept_direct_sender>);
  static_assert(bexec::sender<connect_sender>);
  static_assert(bexec::sender<connect_direct_sender>);
  static_assert(bexec::sender<descriptor_read_sender>);
  static_assert(bexec::sender<descriptor_read_some_sender>);
  static_assert(bexec::sender<descriptor_read_direct_sender>);
  static_assert(bexec::sender<descriptor_read_some_direct_sender>);
  static_assert(bexec::sender<descriptor_write_sender>);
  static_assert(bexec::sender<descriptor_write_some_sender>);
  static_assert(bexec::sender<descriptor_write_direct_sender>);
  static_assert(bexec::sender<descriptor_write_some_direct_sender>);
  static_assert(bexec::sender<poll_sender>);
  static_assert(bexec::sender<schedule_sender>);
  static_assert(bexec::sender<timer_wait_sender>);
  static_assert(bexec::scheduler<bupp::io_context::dispatch_scheduler>);
  static_assert(bexec::scheduler<bupp::io_context::post_scheduler>);
  static_assert(
      !scheduler_can_read_stream<bupp::io_context::post_scheduler,
                                 bupp::tcp_socket, bupp::mutable_buffer>);
  static_assert(
      !scheduler_can_write_stream<bupp::io_context::post_scheduler,
                                  bupp::tcp_socket, bupp::const_buffer>);
  static_assert(bupp::reads_bytes<bupp::io_context::post_scheduler,
                                  bupp::tcp_socket, bupp::mutable_buffer>);
  static_assert(bupp::reads_bytes<bupp::io_context::dispatch_scheduler,
                                  bupp::tcp_socket, bupp::mutable_buffer>);
  static_assert(bupp::writes_bytes<bupp::io_context::post_scheduler,
                                   bupp::tcp_socket, bupp::const_buffer>);
  static_assert(bupp::writes_bytes<bupp::io_context::dispatch_scheduler,
                                   bupp::tcp_socket, bupp::const_buffer>);
  static_assert(bupp::accepts_connections<bupp::io_context::post_scheduler,
                                          bupp::tcp_acceptor>);
  static_assert(
      bupp::connects_stream<bupp::io_context::post_scheduler, bupp::tcp_socket,
                            const bupp::ip::endpoint&>);
  static_assert(
      bupp::reads_bytes<bupp::io_context::post_scheduler,
                        bupp::async_io::descriptor_view, bupp::mutable_buffer>);
  static_assert(
      bupp::writes_bytes<bupp::io_context::post_scheduler,
                         bupp::async_io::descriptor_view, bupp::const_buffer>);
  static_assert(bupp::polls_descriptor<bupp::io_context::post_scheduler,
                                       bupp::async_io::descriptor_view>);

  byte_receiver receiver;
  auto sender = socket.async_read(scheduler, bupp::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  static_assert(bexec::operation_state<decltype(operation)>);

  (void)socket.release();
}

}  // namespace

int main() {
  test_sender_concepts();
  return 0;
}
