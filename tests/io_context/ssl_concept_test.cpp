#include "ssl_test_support.h"

namespace {

void test_ssl_sender_concepts() {
  using stream_type = bupp::ssl_stream<bupp::tcp_socket>;
  using scheduler_type = bupp::io_context::post_scheduler;
  using handshake_sender =
      decltype(std::declval<stream_type&>().async_handshake(
          std::declval<scheduler_type>(), bupp::ssl_handshake_type::client));
  using shutdown_sender = decltype(std::declval<stream_type&>().async_shutdown(
      std::declval<scheduler_type>()));
  using read_sender = decltype(std::declval<stream_type&>().async_read(
      std::declval<scheduler_type>(), std::declval<bupp::mutable_buffer>()));
  using read_some_sender =
      decltype(std::declval<stream_type&>().async_read_some(
          std::declval<scheduler_type>(),
          std::declval<bupp::mutable_buffer>()));
  using write_sender = decltype(std::declval<stream_type&>().async_write(
      std::declval<scheduler_type>(), std::declval<bupp::const_buffer>()));
  using write_some_sender =
      decltype(std::declval<stream_type&>().async_write_some(
          std::declval<scheduler_type>(), std::declval<bupp::const_buffer>()));

  static_assert(bexec::sender<handshake_sender>);
  static_assert(bexec::sender<shutdown_sender>);
  static_assert(bexec::sender<read_sender>);
  static_assert(bexec::sender<read_some_sender>);
  static_assert(bexec::sender<write_sender>);
  static_assert(bexec::sender<write_some_sender>);

  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<handshake_sender>(), void_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<shutdown_sender>(), void_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<read_sender>(), byte_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<read_some_sender>(), byte_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<write_sender>(), byte_receiver{}))>);
  static_assert(bexec::operation_state<decltype(bexec::connect(
                    std::declval<write_some_sender>(), byte_receiver{}))>);
}

void test_ssl_raii_objects_construct() {
  bupp::ssl_context context;
  assert(context.valid());
  context.set_verify_mode(SSL_VERIFY_NONE);

  bupp::tcp_socket socket(-1);
  bupp::ssl_stream stream(std::move(socket), context);
  assert(stream.valid());
  assert(stream.native_handle() == stream.get_native_handle());
  assert(stream.lowest_layer().get_native_handle() == -1);
}

}  // namespace

int main() {
  test_ssl_sender_concepts();
  test_ssl_raii_objects_construct();
  return 0;
}
