/**
 * @file ssl_sni_test.cpp
 * @brief Integration tests for SSL SNI servername callbacks.
 *
 * These tests exercise the public surface only: set_servername_callback, the
 * raw set_tlsext_servername_callback/set_tlsext_servername_arg setters, and
 * the sni_out proxy. The tests are written before the P1 implementation, so
 * they are expected to fail against the skeleton.
 */

#include <bnio/local.h>
#include <bnio/ssl.h>
#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../../support/io_context/ssl_test_support.h"

namespace {

// Wire-format ALPN protocol list for ["h2", "http/1.1"], as consumed by
// SSL_set_alpn_protos and forwarded verbatim to the server callback.
constexpr std::array<unsigned char, 12> k_client_protos = {
    0x02, 'h', '2', 0x08, 'h', 't', 't', 'p', '/', '1', '.', '1'};

constexpr std::string_view k_h2 = "h2";
constexpr std::string_view k_http11 = "http/1.1";
constexpr const char* k_offered_name = "sni.example.test";

using stream_type = bnio::ssl::tcp::stream<bnio::local::stream_socket>;

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

// Asserts the negotiated ALPN protocol on one peer's SSL connection. An empty
// expected value means "no protocol selected".
void expect_selected_alpn(bnio::ssl::stream_view ssl,
                          std::string_view expected) {
  const unsigned char* data = nullptr;
  unsigned int len = 0;
  SSL_get0_alpn_selected(ssl.native_handle(), &data, &len);
  if (expected.empty()) {
    EXPECT_EQ(len, 0u);
    return;
  }
  EXPECT_EQ(len, static_cast<unsigned int>(expected.size()));
  if (len == expected.size()) {
    EXPECT_EQ(std::memcmp(data, expected.data(), len), 0);
  }
}

// Records what the high-level (set_servername_callback) callback observed.
struct sni_probe {
  bool invoked = false;
  std::string seen_servername;
  bnio::ssl::context_view seen_context;
  SSL_CTX* connection_context_handle = nullptr;
  bool connection_matches_context = false;
  bool is_server_connection = false;
  int bound_magic = 0;
  std::string bound_label;
  std::error_code switch_error;
  SSL_CTX* handle_after_switch = nullptr;
};

// State passed to the raw (set_tlsext_servername_callback) native callback.
struct native_sni_state {
  std::string observed_name;
  bool invoked = false;
  int bound_magic = 0;
};

// Captures alert records received by the client so tests can prove which
// alert the peer actually sent. OpenSSL invokes the message callback for both
// received and sent records; only received (write_p == 0) alerts are kept.
struct alert_capture {
  std::vector<int> received;
};

void alert_message_callback(int write_p, int /*version*/, int content_type,
                            const void* buf, std::size_t len, SSL* /*ssl*/,
                            void* arg) {
  if (write_p != 0 || content_type != SSL3_RT_ALERT || len < 2) {
    return;
  }
  const auto* bytes = static_cast<const unsigned char*>(buf);
  static_cast<alert_capture*>(arg)->received.push_back(bytes[1]);
}

[[nodiscard]] bool alerts_contain(const alert_capture& capture, int alert) {
  return std::find(capture.received.begin(), capture.received.end(), alert) !=
         capture.received.end();
}

// Receiver that does not stop the context on the first terminal signal. A
// fatal server callback makes both handshakes fail; both completions must
// arrive before stop(), otherwise the peer's handshake stays inflight forever.
struct dual_terminal_receiver {
  std::shared_ptr<handshake_state> state;
  bnio::io_context* context = nullptr;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      ++state->errors;
      state->error = ec;
    } else {
      ++state->values;
    }
    finish_if_done();
  }

  void set_stopped() noexcept {
    ++state->stopped;
    finish_if_done();
  }

 private:
  void finish_if_done() noexcept {
    if (context != nullptr &&
        state->errors + state->values + state->stopped == 2) {
      (void)context->stop();
    }
  }
};

// Creates a socketpair-backed client/server stream pair with the offered SNI
// name and ALPN protocol list configured on the client.
struct stream_pair {
  int sockets[2] = {-1, -1};
  std::unique_ptr<stream_type> client;
  std::unique_ptr<stream_type> server;

  stream_pair(bnio::ssl::context& client_context,
              bnio::ssl::context& server_context, const char* offered_name,
              std::span<const unsigned char> client_protos) {
    test_require(
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    client = std::make_unique<stream_type>(
        bnio::local::stream_socket(sockets[0]), client_context);
    server = std::make_unique<stream_type>(
        bnio::local::stream_socket(sockets[1]), server_context);
    if (offered_name != nullptr) {
      test_require(SSL_set_tlsext_host_name(
                       client->native_handle().native_handle(), offered_name) ==
                   1);
    }
    if (!client_protos.empty()) {
      // SSL_set_alpn_protos returns 0 on success (inverted convention).
      test_require(SSL_set_alpn_protos(
                       client->native_handle().native_handle(),
                       client_protos.data(),
                       static_cast<unsigned int>(client_protos.size())) == 0);
    }
  }
};

// Drives a successful client/server handshake and asserts the negotiated
// protocol on both peers before their streams are destroyed.
std::shared_ptr<handshake_state> run_success_handshake(
    bnio::io_context& context, bnio::ssl::context& client_context,
    bnio::ssl::context& server_context, const char* offered_name,
    std::span<const unsigned char> client_protos, std::string_view expected) {
  stream_pair pair(client_context, server_context, offered_name, client_protos);

  auto state = std::make_shared<handshake_state>();
  auto scheduler = context.get_post_scheduler();

  auto client_operation =
      bexec::connect(pair.client->async_handshake(
                         scheduler, bnio::ssl::handshake_type::client),
                     handshake_receiver{state, &context});
  auto server_operation =
      bexec::connect(pair.server->async_handshake(
                         scheduler, bnio::ssl::handshake_type::server),
                     handshake_receiver{state, &context});
  bexec::start(client_operation);
  bexec::start(server_operation);
  context.run();

  expect_selected_alpn(pair.client->native_handle(), expected);
  expect_selected_alpn(pair.server->native_handle(), expected);
  return state;
}

// Drives a handshake expected to abort and lets both terminal signals arrive.
std::shared_ptr<handshake_state> run_failing_handshake(
    bnio::io_context& context, bnio::ssl::context& client_context,
    bnio::ssl::context& server_context, const char* offered_name) {
  stream_pair pair(client_context, server_context, offered_name, {});

  auto state = std::make_shared<handshake_state>();
  auto scheduler = context.get_post_scheduler();

  auto client_operation =
      bexec::connect(pair.client->async_handshake(
                         scheduler, bnio::ssl::handshake_type::client),
                     dual_terminal_receiver{state, &context});
  auto server_operation =
      bexec::connect(pair.server->async_handshake(
                         scheduler, bnio::ssl::handshake_type::server),
                     dual_terminal_receiver{state, &context});
  bexec::start(client_operation);
  bexec::start(server_operation);
  context.run();
  return state;
}

// Scenario 1: the callback observes the offered servername, the connection
// and its context, and every bound argument verbatim.
TEST(SslSniTest, callback_observes_servername_and_bound_arguments) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  load_server_identity(server_context, files);

  sni_probe probe;
  server_context.set_servername_callback(
      [](bnio::ssl::context_view ctx, bnio::ssl::stream_view ssl,
         bnio::ssl::sni_out& /*out*/, int magic, std::string label,
         sni_probe* observed) -> int {
        observed->invoked = true;
        observed->seen_context = ctx;
        const char* name = ssl.servername();
        observed->seen_servername = name != nullptr ? name : "";
        observed->connection_context_handle =
            SSL_get_SSL_CTX(ssl.native_handle());
        observed->connection_matches_context =
            observed->connection_context_handle == ctx.native_handle();
        observed->is_server_connection =
            SSL_is_server(ssl.native_handle()) != 0;
        observed->bound_magic = magic;
        observed->bound_label = std::move(label);
        return SSL_TLSEXT_ERR_OK;
      },
      42, std::string("bound-label"), &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  auto state = run_success_handshake(context, client_context, server_context,
                                     k_offered_name, {}, "");
  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(probe.seen_servername, k_offered_name);
  EXPECT_EQ(probe.seen_context.native_handle(), server_context.native_handle());
  EXPECT_TRUE(probe.connection_matches_context);
  EXPECT_TRUE(probe.is_server_connection);
  EXPECT_EQ(probe.bound_magic, 42);
  EXPECT_EQ(probe.bound_label, "bound-label");
}

// Scenario 2: the callback switches the connection's SSL_CTX to a second
// context; the second context's ALPN callback selects a different protocol,
// which proves the switch took effect before ALPN selection.
TEST(SslSniTest, set_context_switch_selects_second_context_alpn) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context first_context(bnio::ssl::context_method::tls_server);
  load_server_identity(first_context, files);
  bnio::ssl::context second_context(bnio::ssl::context_method::tls_server);
  load_server_identity(second_context, files);

  int first_alpn_calls = 0;
  int second_alpn_calls = 0;
  first_context.set_alpn_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view,
         bnio::ssl::context::alpn_out& out, std::span<const unsigned char>,
         int* calls) -> int {
        ++*calls;
        out.set(reinterpret_cast<const unsigned char*>(k_h2.data()),
                static_cast<unsigned char>(k_h2.size()));
        return SSL_TLSEXT_ERR_OK;
      },
      &first_alpn_calls);
  second_context.set_alpn_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view,
         bnio::ssl::context::alpn_out& out, std::span<const unsigned char>,
         int* calls) -> int {
        ++*calls;
        out.set(reinterpret_cast<const unsigned char*>(k_http11.data()),
                static_cast<unsigned char>(k_http11.size()));
        return SSL_TLSEXT_ERR_OK;
      },
      &second_alpn_calls);

  sni_probe probe;
  first_context.set_servername_callback(
      [](bnio::ssl::context_view ctx, bnio::ssl::stream_view ssl,
         bnio::ssl::sni_out& out, bnio::ssl::context* replacement,
         sni_probe* observed) -> int {
        observed->invoked = true;
        observed->seen_context = ctx;
        observed->seen_servername =
            ssl.servername() != nullptr ? ssl.servername() : "";
        observed->switch_error = out.set_context(replacement->view());
        observed->handle_after_switch = SSL_get_SSL_CTX(ssl.native_handle());
        return SSL_TLSEXT_ERR_OK;
      },
      &second_context, &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  auto state = run_success_handshake(context, client_context, first_context,
                                     k_offered_name, k_client_protos, k_http11);
  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(probe.seen_servername, k_offered_name);
  EXPECT_FALSE(probe.switch_error);
  EXPECT_EQ(probe.handle_after_switch, second_context.native_handle());
  // The switch happened before ALPN selection: only the second context's
  // selector ran, and its protocol won.
  EXPECT_EQ(first_alpn_calls, 0);
  EXPECT_EQ(second_alpn_calls, 1);
}

// Scenario 2b: SSL_set_SSL_CTX duplicates the target context's certificate
// callback registration into the connection, so the switched-to context's
// high-level certificate callback must run for this handshake.
TEST(SslSniTest, set_context_switch_runs_second_context_cert_callback) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context first_context(bnio::ssl::context_method::tls_server);
  load_server_identity(first_context, files);
  bnio::ssl::context second_context(bnio::ssl::context_method::tls_server);
  load_server_identity(second_context, files);

  int cert_calls = 0;
  second_context.set_cert_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view, bnio::ssl::cert_out&,
         int* calls) -> bnio::ssl::cert_result {
        ++*calls;
        return bnio::ssl::cert_result::ok;
      },
      &cert_calls);

  sni_probe switch_probe;
  first_context.set_servername_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view,
         bnio::ssl::sni_out& out, bnio::ssl::context* replacement,
         sni_probe* observed) -> int {
        observed->invoked = true;
        observed->switch_error = out.set_context(replacement->view());
        return SSL_TLSEXT_ERR_OK;
      },
      &second_context, &switch_probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  auto state = run_success_handshake(context, client_context, first_context,
                                     k_offered_name, {}, "");
  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  EXPECT_TRUE(switch_probe.invoked);
  EXPECT_FALSE(switch_probe.switch_error);
  EXPECT_EQ(cert_calls, 1);
}

// Scenario 3: returning SSL_TLSEXT_ERR_ALERT_FATAL after recording an alert
// fails the handshake, and the peer receives exactly that alert.
TEST(SslSniTest, alert_fatal_fails_handshake_and_peer_receives_alert) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  load_server_identity(server_context, files);

  sni_probe probe;
  server_context.set_servername_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view,
         bnio::ssl::sni_out& out, sni_probe* observed) -> int {
        observed->invoked = true;
        out.set_alert(SSL_AD_ACCESS_DENIED);
        return SSL_TLSEXT_ERR_ALERT_FATAL;
      },
      &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  alert_capture capture;
  SSL_CTX_set_msg_callback(client_context.native_handle(),
                           &alert_message_callback);
  SSL_CTX_set_msg_callback_arg(client_context.native_handle(), &capture);

  auto state = run_failing_handshake(context, client_context, server_context,
                                     k_offered_name);
  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(state->values, 0);
  EXPECT_EQ(state->errors, 2);
  EXPECT_EQ(state->stopped, 0);
  EXPECT_TRUE(state->error);
  EXPECT_TRUE(alerts_contain(capture, SSL_AD_ACCESS_DENIED))
      << "the client did not receive the alert recorded by the callback";
}

// Scenario 4: a throwing callback fails closed with SSL_AD_INTERNAL_ERROR and
// no exception crosses the OpenSSL C boundary.
TEST(SslSniTest, throwing_callback_fails_closed_with_internal_error) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  load_server_identity(server_context, files);

  sni_probe probe;
  server_context.set_servername_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view, bnio::ssl::sni_out&,
         sni_probe* observed) -> int {
        observed->invoked = true;
        throw std::runtime_error("servername callback failure");
      },
      &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  alert_capture capture;
  SSL_CTX_set_msg_callback(client_context.native_handle(),
                           &alert_message_callback);
  SSL_CTX_set_msg_callback_arg(client_context.native_handle(), &capture);

  auto state = run_failing_handshake(context, client_context, server_context,
                                     k_offered_name);
  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(state->values, 0);
  EXPECT_EQ(state->errors, 2);
  EXPECT_EQ(state->stopped, 0);
  EXPECT_TRUE(state->error);
  EXPECT_TRUE(alerts_contain(capture, SSL_AD_INTERNAL_ERROR))
      << "the fail-closed alert did not reach the peer";
}

// Scenario 5: the raw set_tlsext_servername_callback + arg setters round-trip
// the function pointer and void* argument to the native callback.
TEST(SslSniTest, native_servername_callback_round_trips_arg) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  load_server_identity(server_context, files);

  native_sni_state state;
  server_context.set_tlsext_servername_callback(
      [](SSL* ssl, int* /*al*/, void* arg) -> int {
        auto* observed = static_cast<native_sni_state*>(arg);
        observed->invoked = true;
        const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
        observed->observed_name = name != nullptr ? name : "";
        return SSL_TLSEXT_ERR_OK;
      });
  server_context.set_tlsext_servername_arg(&state);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  auto outcome = run_success_handshake(context, client_context, server_context,
                                       k_offered_name, {}, "");
  ASSERT_EQ(outcome->values, 2);
  EXPECT_EQ(outcome->errors, 0);
  EXPECT_EQ(outcome->stopped, 0);

  EXPECT_TRUE(state.invoked);
  EXPECT_EQ(state.observed_name, k_offered_name);
}

// Scenario 6: moving a context after install keeps both the native
// registration and the heap closure, through move construction and move
// assignment.
TEST(SslSniTest, context_move_after_install_keeps_callback) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context original(bnio::ssl::context_method::tls_server);
  load_server_identity(original, files);

  sni_probe probe;
  original.set_servername_callback(
      [](bnio::ssl::context_view ctx, bnio::ssl::stream_view,
         bnio::ssl::sni_out&, sni_probe* observed) -> int {
        observed->invoked = true;
        observed->seen_context = ctx;
        return SSL_TLSEXT_ERR_OK;
      },
      &probe);

  bnio::ssl::context moved(std::move(original));
  EXPECT_FALSE(original.valid());
  ASSERT_TRUE(moved.valid());

  bnio::ssl::context assigned(bnio::ssl::context_method::tls_server);
  ASSERT_TRUE(assigned.valid());
  assigned = std::move(moved);
  EXPECT_FALSE(moved.valid());
  ASSERT_TRUE(assigned.valid());

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  auto state = run_success_handshake(context, client_context, assigned,
                                     k_offered_name, {}, "");
  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(probe.seen_context.native_handle(), assigned.native_handle());
}

// Scenario 7: destroying the owning context before the handshake clears the
// native registration; the handshake still succeeds from the native SSL_CTX
// the SSL object keeps alive, and the callback never runs.
TEST(SslSniTest, context_destroyed_before_handshake_disables_callback) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  auto server_context = std::make_unique<bnio::ssl::context>(
      bnio::ssl::context_method::tls_server);
  load_server_identity(*server_context, files);

  sni_probe probe;
  server_context->set_servername_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view, bnio::ssl::sni_out&,
         sni_probe* observed) -> int {
        observed->invoked = true;
        return SSL_TLSEXT_ERR_OK;
      },
      &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  // Control: while the owner is alive, the installed callback runs.
  auto control_state = run_success_handshake(
      context, client_context, *server_context, k_offered_name, {}, "");
  ASSERT_EQ(control_state->values, 2);
  ASSERT_EQ(control_state->errors, 0);
  ASSERT_TRUE(probe.invoked);
  probe.invoked = false;

  stream_pair pair(client_context, *server_context, k_offered_name, {});
  ASSERT_TRUE(pair.server->valid());
  SSL_CTX* const native_context = server_context->native_handle();
  ASSERT_EQ(SSL_get_SSL_CTX(pair.server->native_handle().native_handle()),
            native_context);

  // The bnio owner (and with it the owned closure) is gone before the
  // handshake; the native SSL_CTX survives through the SSL object's reference.
  server_context.reset();

  // The control handshake stopped the first io_context, and a stopped
  // context cancels every later run (io_context::stop() is latched), so this
  // handshake needs a fresh event loop.
  bnio::io_context second_context;
  if (!second_context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  auto state = std::make_shared<handshake_state>();
  auto scheduler = second_context.get_post_scheduler();
  auto client_operation =
      bexec::connect(pair.client->async_handshake(
                         scheduler, bnio::ssl::handshake_type::client),
                     handshake_receiver{state, &second_context});
  auto server_operation =
      bexec::connect(pair.server->async_handshake(
                         scheduler, bnio::ssl::handshake_type::server),
                     handshake_receiver{state, &second_context});
  bexec::start(client_operation);
  bexec::start(server_operation);
  second_context.run();

  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);
  EXPECT_FALSE(probe.invoked);
}

// Scenario 7b: switching to a second bnio context and destroying that owner
// from inside the SNI callback is safe for dynamically looked-up callbacks.
// The SSL object keeps the native SSL_CTX alive through its own reference, the
// destroyed owner's registrations are cleared, and the handshake proceeds with
// the target's default certificate. A target with a copied-argument callback
// (currently cert_cb) would instead have to outlive the stream.
TEST(SslSniTest,
     switched_context_destroyed_in_callback_keeps_handshake_working) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context first_context(bnio::ssl::context_method::tls_server);
  load_server_identity(first_context, files);

  auto target_context = std::make_unique<bnio::ssl::context>(
      bnio::ssl::context_method::tls_server);
  load_server_identity(*target_context, files);

  sni_probe switch_probe;
  first_context.set_servername_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view,
         bnio::ssl::sni_out& out, std::unique_ptr<bnio::ssl::context>* target,
         sni_probe* observed) -> int {
        observed->invoked = true;
        observed->switch_error = out.set_context((*target)->view());
        // The bnio owner (and with it the owned closures) is destroyed from
        // inside the handshake. The SSL keeps the native SSL_CTX alive.
        target->reset();
        return SSL_TLSEXT_ERR_OK;
      },
      &target_context, &switch_probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  auto state = run_success_handshake(context, client_context, first_context,
                                     k_offered_name, {}, "");
  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  EXPECT_TRUE(switch_probe.invoked);
  EXPECT_FALSE(switch_probe.switch_error);
}

// Scenario 8: installing a second callback replaces the first; only the new
// closure runs.
TEST(SslSniTest, replacement_installs_only_latest_callback) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  load_server_identity(server_context, files);

  sni_probe first_probe;
  sni_probe second_probe;
  server_context.set_servername_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view, bnio::ssl::sni_out&,
         sni_probe* observed) -> int {
        observed->invoked = true;
        return SSL_TLSEXT_ERR_OK;
      },
      &first_probe);
  server_context.set_servername_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view, bnio::ssl::sni_out&,
         sni_probe* observed) -> int {
        observed->invoked = true;
        return SSL_TLSEXT_ERR_OK;
      },
      &second_probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  auto state = run_success_handshake(context, client_context, server_context,
                                     k_offered_name, {}, "");
  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  EXPECT_FALSE(first_probe.invoked);
  EXPECT_TRUE(second_probe.invoked);
}

}  // namespace
