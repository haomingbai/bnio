#include <gtest/gtest.h>
#include <openssl/err.h>

#include "../../support/io_context/ssl_test_support.h"

namespace {

TEST(SslHandshakeTest, socketpair_handshake_is_io_context_driven) {
  bupp::io_context context;
  if (!context.is_open()) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  test_certificate_files files;

  bupp::ssl_context server_context(bupp::ssl_context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());

  bupp::ssl_context client_context(bupp::ssl_context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);

  bupp::ssl_stream client{bupp::tcp_socket(sockets[0]), client_context};
  bupp::ssl_stream server{bupp::tcp_socket(sockets[1]), server_context};

  auto state = std::make_shared<handshake_state>();
  handshake_receiver client_receiver{state, &context};
  handshake_receiver server_receiver{state, &context};

  auto client_sender =
      client.async_handshake(scheduler, bupp::ssl_handshake_type::client);
  auto server_sender =
      server.async_handshake(scheduler, bupp::ssl_handshake_type::server);

  auto client_operation =
      bexec::connect(std::move(client_sender), std::move(client_receiver));
  auto server_operation =
      bexec::connect(std::move(server_sender), std::move(server_receiver));

  bexec::start(client_operation);
  bexec::start(server_operation);
  context.run();

  EXPECT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);
}

TEST(SslHandshakeTest, invalid_stream_reports_protocol_error) {
  bupp::io_context context;
  if (!context.is_open()) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  bupp::ssl_context ssl_context(bupp::ssl_context_method::tls_client);
  test_require(ssl_context.valid());
  bupp::ssl_stream source{bupp::tcp_socket(-1), ssl_context};
  bupp::ssl_stream owner{std::move(source)};
  EXPECT_FALSE(source.valid());
  EXPECT_TRUE(owner.valid());

  ERR_clear_error();
  auto state = std::make_shared<handshake_state>();
  auto sender =
      source.async_handshake(scheduler, bupp::ssl_handshake_type::client);
  auto operation =
      bexec::connect(std::move(sender), handshake_receiver{state, &context});
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->values, 0);
  EXPECT_EQ(state->errors, 1);
  EXPECT_EQ(state->stopped, 0);
  EXPECT_TRUE(state->error == std::errc::protocol_error);
}

TEST(SslHandshakeTest, closed_transport_reports_handshake_error) {
  bupp::io_context context;
  if (!context.is_open()) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  bupp::ssl_context ssl_context(bupp::ssl_context_method::tls_client);
  test_require(ssl_context.valid());
  ssl_context.set_verify_mode(SSL_VERIFY_NONE);

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);
  test_require(::close(sockets[1]) == 0);
  bupp::ssl_stream stream{bupp::tcp_socket(sockets[0]), ssl_context};

  auto state = std::make_shared<handshake_state>();
  auto sender =
      stream.async_handshake(scheduler, bupp::ssl_handshake_type::client);
  auto operation =
      bexec::connect(std::move(sender), handshake_receiver{state, &context});
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->values, 0);
  EXPECT_EQ(state->errors, 1);
  EXPECT_EQ(state->stopped, 0);
  EXPECT_TRUE(state->error);
}

TEST(SslHandshakeTest, socketpair_shutdown_exchanges_close_notify) {
  test_certificate_files files;

  bupp::ssl_context server_context(bupp::ssl_context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());

  bupp::ssl_context client_context(bupp::ssl_context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);
  bupp::ssl_stream client{bupp::tcp_socket(sockets[0]), client_context};
  bupp::ssl_stream server{bupp::tcp_socket(sockets[1]), server_context};

  {
    bupp::io_context context;
    if (!context.is_open()) {
      return;
    }
    auto scheduler = context.get_post_scheduler();
    auto state = std::make_shared<handshake_state>();
    auto client_operation = bexec::connect(
        client.async_handshake(scheduler, bupp::ssl_handshake_type::client),
        handshake_receiver{state, &context});
    auto server_operation = bexec::connect(
        server.async_handshake(scheduler, bupp::ssl_handshake_type::server),
        handshake_receiver{state, &context});
    bexec::start(client_operation);
    bexec::start(server_operation);
    context.run();
    EXPECT_EQ(state->values, 2);
    EXPECT_EQ(state->errors, 0);
    EXPECT_EQ(state->stopped, 0);
  }

  bupp::io_context context;
  if (!context.is_open()) {
    return;
  }
  auto scheduler = context.get_post_scheduler();
  auto state = std::make_shared<handshake_state>();
  auto client_operation = bexec::connect(client.async_shutdown(scheduler),
                                         handshake_receiver{state, &context});
  auto server_operation = bexec::connect(server.async_shutdown(scheduler),
                                         handshake_receiver{state, &context});
  bexec::start(client_operation);
  bexec::start(server_operation);
  context.run();

  EXPECT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);
}

}  // namespace
