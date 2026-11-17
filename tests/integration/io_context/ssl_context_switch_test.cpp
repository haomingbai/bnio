#include <bnio/local.h>
#include <bnio/ssl.h>
#include <gtest/gtest.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "../../support/io_context/ssl_test_support.h"

namespace {

// Wire-format ALPN protocol list for ["h2", "http/1.1"], as consumed by
// SSL_set_alpn_protos and forwarded verbatim to the server callback.
constexpr std::array<unsigned char, 12> k_client_protos = {
    0x02, 'h', '2', 0x08, 'h', 't', 't', 'p', '/', '1', '.', '1'};

constexpr std::string_view k_h2 = "h2";
constexpr std::string_view k_http11 = "http/1.1";

// Reinterprets a negotiated-protocol byte span as a string_view so tests can
// compare it directly against the expected protocol name.
std::string_view as_string_view(std::span<const unsigned char> bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()),
                          bytes.size());
}

// Records whether one context's ALPN callback ran during a handshake.
struct alpn_probe {
  bool invoked = false;
};

// Loads the shared test certificate and key into a server context.
void load_server_identity(bnio::ssl::context& context,
                          const test_certificate_files& files) {
  test_require(context.valid());
  test_require(
      !context.use_certificate_chain_file(files.certificate.string().c_str()));
  test_require(
      !context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!context.check_private_key());
}

// Clears the thread-local OpenSSL error queue, seeds one deterministic entry,
// and returns the code ERR_peek_error() reports. A path that never reaches
// OpenSSL leaves this entry intact; a path that calls clear_errors() before
// OpenSSL removes it.
unsigned long seed_error_queue() {
  ERR_clear_error();
  ERR_put_error(ERR_LIB_USER, 0, 1234, "bnio_ssl_context_switch_test.c", 1);
  const unsigned long seeded = ERR_peek_error();
  test_require(seeded != 0);
  return seeded;
}

// Drives the context event loop. Tests assert the operation outcomes, so the
// run() status (empty on a normal run, operation_canceled after a receiver
// requests stop()) is intentionally discarded.
void run_context(bnio::io_context& context) {
  const std::error_code run_error = context.run();
  (void)run_error;
}

// Drives a client/server handshake for streams constructed by the caller and
// returns the shared outcome state. The streams outlive this call, so tests
// can query them afterwards.
template <class ClientStream, class ServerStream>
std::shared_ptr<handshake_state> handshake_streams(bnio::io_context& context,
                                                   ClientStream& client,
                                                   ServerStream& server) {
  auto state = std::make_shared<handshake_state>();
  auto scheduler = context.get_post_scheduler();

  auto client_operation = bexec::connect(
      client.async_handshake(scheduler, bnio::ssl::handshake_type::client),
      handshake_receiver{state, &context});
  auto server_operation = bexec::connect(
      server.async_handshake(scheduler, bnio::ssl::handshake_type::server),
      handshake_receiver{state, &context});
  bexec::start(client_operation);
  bexec::start(server_operation);
  run_context(context);
  return state;
}

// Scenario 1: SSL_set_SSL_CTX before the handshake adopts the new context's
// ALPN callback (the old context's callback must not run) and the handshake
// completes with the new callback's selection.
TEST(SslContextSwitchTest,
     pre_handshake_switch_uses_new_context_alpn_callback) {
  bnio::io_context context;
  if (!context.is_open()) {
    return;
  }

  test_certificate_files files;

  bnio::ssl::context context_a(bnio::ssl::context_method::tls_server);
  load_server_identity(context_a, files);
  bnio::ssl::context context_b(bnio::ssl::context_method::tls_server);
  load_server_identity(context_b, files);

  alpn_probe probe_a;
  alpn_probe probe_b;
  context_a.set_alpn_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view,
         bnio::ssl::context::alpn_out& out, std::span<const unsigned char>,
         alpn_probe* probe) -> int {
        probe->invoked = true;
        out.set(reinterpret_cast<const unsigned char*>(k_http11.data()),
                static_cast<unsigned char>(k_http11.size()));
        return SSL_TLSEXT_ERR_OK;
      },
      &probe_a);
  context_b.set_alpn_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view,
         bnio::ssl::context::alpn_out& out, std::span<const unsigned char>,
         alpn_probe* probe) -> int {
        probe->invoked = true;
        out.set(reinterpret_cast<const unsigned char*>(k_h2.data()),
                static_cast<unsigned char>(k_h2.size()));
        return SSL_TLSEXT_ERR_OK;
      },
      &probe_b);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);

  bnio::ssl::tcp::stream client{bnio::local::stream_socket(sockets[0]),
                                client_context};
  bnio::ssl::tcp::stream server{bnio::local::stream_socket(sockets[1]),
                                context_a};

  ASSERT_TRUE(server.valid());
  ASSERT_EQ(SSL_get_SSL_CTX(server.native_handle().native_handle()),
            context_a.native_handle());

  const std::error_code switch_error = server.set_context(context_b);
  EXPECT_FALSE(switch_error);
  EXPECT_EQ(switch_error, bnio::ssl::base::empty_error_code);
  ASSERT_EQ(SSL_get_SSL_CTX(server.native_handle().native_handle()),
            context_b.native_handle());

  // SSL_set_alpn_protos returns 0 on success (inverted convention).
  test_require(SSL_set_alpn_protos(
                   client.native_handle().native_handle(),
                   k_client_protos.data(),
                   static_cast<unsigned int>(k_client_protos.size())) == 0);

  auto state = handshake_streams(context, client, server);
  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  // The new context's callback ran; the old context's callback never did.
  EXPECT_TRUE(probe_b.invoked);
  EXPECT_FALSE(probe_a.invoked);
  EXPECT_EQ(as_string_view(client.get_alpn_selected()), k_h2);
  EXPECT_EQ(as_string_view(server.get_alpn_selected()), k_h2);
}

// Scenario 2: an invalid stream (moved-from) reports the dedicated
// no-OpenSSL-error value, never calls OpenSSL, and leaves the thread-local
// error queue untouched.
TEST(SslContextSwitchTest, moved_from_stream_reports_no_ssl_error) {
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  test_require(ssl_context.valid());

  bnio::ssl::tcp::stream source{bnio::tcp_socket(-1), ssl_context};
  bnio::ssl::tcp::stream owner{std::move(source)};
  EXPECT_FALSE(source.valid());
  EXPECT_TRUE(owner.valid());

  const unsigned long seeded = seed_error_queue();
  const std::error_code error = source.set_context(ssl_context);
  EXPECT_EQ(error, bnio::ssl::make_no_ssl_error());
  // The invalid-stream path never reached OpenSSL, so the seeded entry is
  // still exactly what a queue reader sees.
  EXPECT_EQ(ERR_peek_error(), seeded);
  ERR_clear_error();
}

// Scenario 3: an invalid context (moved-from) is rejected before OpenSSL is
// called; the current context stays installed. Re-passing the current context
// is a successful no-op.
TEST(SslContextSwitchTest, invalid_context_rejected_and_current_context_kept) {
  bnio::ssl::context current(bnio::ssl::context_method::tls_server);
  bnio::ssl::context other(bnio::ssl::context_method::tls_server);
  test_require(current.valid());
  test_require(other.valid());
  bnio::ssl::context owner_of_other(std::move(other));
  EXPECT_TRUE(owner_of_other.valid());
  EXPECT_FALSE(other.valid());

  bnio::ssl::tcp::stream server{bnio::tcp_socket(-1), current};
  ASSERT_TRUE(server.valid());

  const unsigned long seeded_before_invalid = seed_error_queue();
  const std::error_code invalid_error = server.set_context(other);
  EXPECT_EQ(invalid_error, std::errc::invalid_argument);
  // Invalid contexts are rejected before OpenSSL: no clear_errors() ran, so
  // the seeded entry is still queued.
  EXPECT_EQ(ERR_peek_error(), seeded_before_invalid);
  EXPECT_EQ(SSL_get_SSL_CTX(server.native_handle().native_handle()),
            current.native_handle());

  // A successful call clears the queue before reaching OpenSSL.
  test_require(seed_error_queue() != 0);
  const std::error_code same_context_error = server.set_context(current);
  EXPECT_FALSE(same_context_error);
  EXPECT_EQ(same_context_error, bnio::ssl::base::empty_error_code);
  EXPECT_EQ(ERR_peek_error(), 0u);
  EXPECT_EQ(SSL_get_SSL_CTX(server.native_handle().native_handle()),
            current.native_handle());
  ERR_clear_error();
}

// Scenario 3b: invalid stream takes priority over invalid context. The stream
// guard runs first, so the result is make_no_ssl_error() and OpenSSL is never
// reached even though both arguments are invalid.
TEST(SslContextSwitchTest, invalid_stream_takes_priority_over_invalid_context) {
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  test_require(ssl_context.valid());
  bnio::ssl::context other(bnio::ssl::context_method::tls_client);
  test_require(other.valid());
  bnio::ssl::context owner_of_other(std::move(other));
  EXPECT_FALSE(other.valid());

  bnio::ssl::tcp::stream source{bnio::tcp_socket(-1), ssl_context};
  bnio::ssl::tcp::stream owner{std::move(source)};
  EXPECT_FALSE(source.valid());

  const unsigned long seeded = seed_error_queue();
  const std::error_code error = source.set_context(other);
  EXPECT_EQ(error, bnio::ssl::make_no_ssl_error());
  EXPECT_EQ(ERR_peek_error(), seeded);
  ERR_clear_error();
}

// Scenario 4: moving the stream after a context switch moves the SSL object
// with the new context attached, and the moved-to stream still handshakes.
TEST(SslContextSwitchTest, move_after_switch_keeps_new_context) {
  bnio::io_context context;
  if (!context.is_open()) {
    return;
  }

  test_certificate_files files;

  bnio::ssl::context context_a(bnio::ssl::context_method::tls_server);
  load_server_identity(context_a, files);
  bnio::ssl::context context_b(bnio::ssl::context_method::tls_server);
  load_server_identity(context_b, files);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);

  bnio::ssl::tcp::stream client{bnio::local::stream_socket(sockets[0]),
                                client_context};
  bnio::ssl::tcp::stream server{bnio::local::stream_socket(sockets[1]),
                                context_a};

  ASSERT_FALSE(server.set_context(context_b));
  ASSERT_EQ(SSL_get_SSL_CTX(server.native_handle().native_handle()),
            context_b.native_handle());

  bnio::ssl::tcp::stream moved_server{std::move(server)};
  EXPECT_FALSE(server.valid());
  ASSERT_TRUE(moved_server.valid());
  ASSERT_EQ(SSL_get_SSL_CTX(moved_server.native_handle().native_handle()),
            context_b.native_handle());

  auto state = handshake_streams(context, client, moved_server);
  EXPECT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);
}

// Scenario 5: switching contexts after the handshake succeeds and updates the
// SSL object's context, but the already established connection keeps working
// in both directions; the switch is not retroactive.
TEST(SslContextSwitchTest, post_handshake_switch_keeps_connection_usable) {
  test_certificate_files files;

  bnio::ssl::context context_a(bnio::ssl::context_method::tls_server);
  load_server_identity(context_a, files);
  bnio::ssl::context context_b(bnio::ssl::context_method::tls_server);
  load_server_identity(context_b, files);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);

  bnio::ssl::tcp::stream client{bnio::local::stream_socket(sockets[0]),
                                client_context};
  bnio::ssl::tcp::stream server{bnio::local::stream_socket(sockets[1]),
                                context_a};

  {
    bnio::io_context handshake_io;
    if (!handshake_io.is_open()) {
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    auto state = handshake_streams(handshake_io, client, server);
    ASSERT_EQ(state->values, 2);
    ASSERT_EQ(state->errors, 0);
    ASSERT_EQ(state->stopped, 0);
  }

  // The non-owning context_view overload stays available for callers that
  // only hold a view; the owning ssl::context overload is covered above.
  const std::error_code switch_error = server.set_context(context_b.view());
  EXPECT_FALSE(switch_error);
  EXPECT_EQ(SSL_get_SSL_CTX(server.native_handle().native_handle()),
            context_b.native_handle());

  const std::string payload = "still connected after switch";
  std::array<char, 32> received{};

  // Client write -> server read.
  {
    bnio::io_context io;
    if (!io.is_open()) {
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    auto scheduler = io.get_post_scheduler();
    unsigned completions = 0;
    auto read_state = std::make_shared<transfer_state>();
    auto write_state = std::make_shared<transfer_state>();
    transfer_receiver read_receiver{read_state, &io, &completions, 2};
    transfer_receiver write_receiver{write_state, &io, &completions, 2};

    auto read_sender = server.async_read(
        scheduler, bnio::buffer(received.data(), payload.size()));
    auto write_sender = client.async_write(
        scheduler, bnio::buffer(payload.data(), payload.size()));

    auto read_operation =
        bexec::connect(std::move(read_sender), std::move(read_receiver));
    auto write_operation =
        bexec::connect(std::move(write_sender), std::move(write_receiver));
    bexec::start(read_operation);
    bexec::start(write_operation);
    run_context(io);

    EXPECT_EQ(completions, 2u);
    EXPECT_EQ(write_state->values, 1u);
    EXPECT_EQ(write_state->errors, 0u);
    EXPECT_EQ(write_state->bytes, payload.size());
    EXPECT_EQ(read_state->values, 1u);
    EXPECT_EQ(read_state->errors, 0u);
    EXPECT_EQ(read_state->bytes, payload.size());
    EXPECT_EQ(std::memcmp(received.data(), payload.data(), payload.size()), 0);
  }

  // Server write -> client read.
  const std::string reply = "server still connected";
  std::array<char, 32> reply_received{};
  {
    bnio::io_context io;
    if (!io.is_open()) {
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    auto scheduler = io.get_post_scheduler();
    unsigned completions = 0;
    auto read_state = std::make_shared<transfer_state>();
    auto write_state = std::make_shared<transfer_state>();
    transfer_receiver read_receiver{read_state, &io, &completions, 2};
    transfer_receiver write_receiver{write_state, &io, &completions, 2};

    auto read_sender = client.async_read(
        scheduler, bnio::buffer(reply_received.data(), reply.size()));
    auto write_sender =
        server.async_write(scheduler, bnio::buffer(reply.data(), reply.size()));

    auto read_operation =
        bexec::connect(std::move(read_sender), std::move(read_receiver));
    auto write_operation =
        bexec::connect(std::move(write_sender), std::move(write_receiver));
    bexec::start(read_operation);
    bexec::start(write_operation);
    run_context(io);

    EXPECT_EQ(completions, 2u);
    EXPECT_EQ(write_state->values, 1u);
    EXPECT_EQ(write_state->errors, 0u);
    EXPECT_EQ(write_state->bytes, reply.size());
    EXPECT_EQ(read_state->values, 1u);
    EXPECT_EQ(read_state->errors, 0u);
    EXPECT_EQ(read_state->bytes, reply.size());
    EXPECT_EQ(std::memcmp(reply_received.data(), reply.data(), reply.size()),
              0);
  }
}

}  // namespace
