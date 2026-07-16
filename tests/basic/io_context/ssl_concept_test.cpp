#include <gtest/gtest.h>
#include <openssl/err.h>

#include "../../support/io_context/ssl_test_support.h"

namespace {

template <class Owner>
void self_move_assign(Owner& owner) {
  owner = std::move(owner);
}

TEST(SslConceptTest, ssl_sender_concepts) {
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

TEST(SslConceptTest, ssl_raii_objects_construct) {
  bupp::ssl_context context;
  EXPECT_TRUE(context.valid());
  context.set_verify_mode(SSL_VERIFY_NONE);

  bupp::tcp_socket socket(-1);
  bupp::ssl_stream stream(std::move(socket), context);
  EXPECT_TRUE(stream.valid());
  EXPECT_EQ(stream.native_handle(), stream.get_native_handle());
  EXPECT_EQ(stream.lowest_layer().get_native_handle(), -1);
}

TEST(SslConceptTest, ssl_context_errors_and_ownership) {
  const std::error_code synthetic_error = bupp::make_openssl_error(1);
  EXPECT_EQ(synthetic_error.value(), 1);
  EXPECT_EQ(synthetic_error.category(), bupp::openssl_error_category());
  EXPECT_EQ(std::string_view(synthetic_error.category().name()), "openssl");
  EXPECT_FALSE(synthetic_error.message().empty());

  bupp::ssl_context generic_context(bupp::ssl_context_method::tls);
  bupp::ssl_context client_context(bupp::ssl_context_method::tls_client);
  bupp::ssl_context server_context(bupp::ssl_context_method::tls_server);
  EXPECT_TRUE(generic_context.valid());
  EXPECT_TRUE(client_context.valid());
  EXPECT_TRUE(server_context.valid());

  SSL_CTX* const client_handle = client_context.native_handle();
  bupp::ssl_context moved_context(std::move(client_context));
  EXPECT_FALSE(client_context.valid());
  EXPECT_EQ(moved_context.native_handle(), client_handle);

  generic_context = std::move(moved_context);
  EXPECT_FALSE(moved_context.valid());
  EXPECT_EQ(generic_context.native_handle(), client_handle);
  self_move_assign(generic_context);
  EXPECT_EQ(generic_context.native_handle(), client_handle);

  ERR_clear_error();
  const std::error_code certificate_error =
      server_context.use_certificate_chain_file(
          "/bupp-test/path-that-does-not-exist/cert.pem");
  EXPECT_TRUE(certificate_error);
  EXPECT_EQ(certificate_error.category(), bupp::openssl_error_category());

  ERR_clear_error();
  const std::error_code key_error = server_context.use_private_key_file(
      "/bupp-test/path-that-does-not-exist/key.pem");
  EXPECT_TRUE(key_error);
  EXPECT_EQ(key_error.category(), bupp::openssl_error_category());

  ERR_clear_error();
  const std::error_code mismatch_error = server_context.check_private_key();
  EXPECT_TRUE(mismatch_error);
  EXPECT_EQ(mismatch_error.category(), bupp::openssl_error_category());
}

}  // namespace
