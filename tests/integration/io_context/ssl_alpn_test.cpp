#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../support/io_context/ssl_test_support.h"

namespace {

// Wire-format ALPN protocol list for ["h2", "http/1.1"], as consumed by
// SSL_set_alpn_protos and forwarded verbatim to the server callback.
constexpr std::array<unsigned char, 12> k_client_protos = {
    0x02, 'h', '2', 0x08, 'h', 't', 't', 'p', '/', '1', '.', '1'};

constexpr std::string_view k_h2 = "h2";
constexpr std::string_view k_http11 = "http/1.1";

// Records what the high-level (set_alpn_callback) callback observed.
struct alpn_probe {
  bool invoked = false;
  bnio::ssl_context* seen_context = nullptr;
  bool wire_match = false;
  int bound_magic = 0;
  std::string seen_protocol;
};

// State passed as the native (set_alpn_select_cb) void* arg.
struct native_selector {
  std::string protocol;
  bool invoked = false;
  std::vector<unsigned char> received;
};

// Native callback; the anonymous namespace keeps internal linkage.
int native_alpn_select(SSL* /*ssl*/, const unsigned char** out,
                       unsigned char* outlen, const unsigned char* in,
                       unsigned int inlen, void* arg) {
  auto* selector = static_cast<native_selector*>(arg);
  selector->invoked = true;
  selector->received.assign(in, in + inlen);
  *out = reinterpret_cast<const unsigned char*>(selector->protocol.data());
  *outlen = static_cast<unsigned char>(selector->protocol.size());
  return SSL_TLSEXT_ERR_OK;
}

// Asserts the negotiated ALPN protocol on one peer's SSL handle. An empty
// expected value means "no protocol selected".
void expect_selected_alpn(SSL* ssl, std::string_view expected) {
  const unsigned char* data = nullptr;
  unsigned int len = 0;
  SSL_get0_alpn_selected(ssl, &data, &len);
  if (expected.empty()) {
    EXPECT_EQ(len, 0u);
    return;
  }
  EXPECT_EQ(len, static_cast<unsigned int>(expected.size()));
  if (len == expected.size()) {
    EXPECT_EQ(std::memcmp(data, expected.data(), len), 0);
  }
}

// Drives a client/server handshake over an AF_UNIX socketpair and asserts the
// negotiated protocol on both peers before the streams are destroyed.
std::shared_ptr<handshake_state> run_socketpair_handshake(
    bnio::io_context& context, bnio::ssl_context& client_ctx,
    bnio::ssl_context& server_ctx, std::span<const unsigned char> client_protos,
    std::string_view expected_alpn) {
  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);

  bnio::ssl_stream client{bnio::tcp_socket(sockets[0]), client_ctx};
  bnio::ssl_stream server{bnio::tcp_socket(sockets[1]), server_ctx};

  if (!client_protos.empty()) {
    // SSL_set_alpn_protos returns 0 on success (inverted convention).
    test_require(SSL_set_alpn_protos(
                     client.native_handle(), client_protos.data(),
                     static_cast<unsigned int>(client_protos.size())) == 0);
  }

  auto state = std::make_shared<handshake_state>();
  auto scheduler = context.get_post_scheduler();

  auto client_operation = bexec::connect(
      client.async_handshake(scheduler, bnio::ssl_handshake_type::client),
      handshake_receiver{state, &context});
  auto server_operation = bexec::connect(
      server.async_handshake(scheduler, bnio::ssl_handshake_type::server),
      handshake_receiver{state, &context});
  bexec::start(client_operation);
  bexec::start(server_operation);
  context.run();

  expect_selected_alpn(client.native_handle(), expected_alpn);
  expect_selected_alpn(server.native_handle(), expected_alpn);
  return state;
}

// Scenario 1: high-level callback selects "h2", receives the exact wire
// bytes, the correct context reference, and the bound arguments verbatim.
TEST(SslAlpnTest, template_callback_selects_h2_and_receives_bindings) {
  bnio::io_context context;
  if (!context.is_open()) {
    return;
  }

  test_certificate_files files;

  bnio::ssl_context server_context(bnio::ssl_context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());

  alpn_probe probe;
  server_context.set_alpn_callback(
      [](bnio::ssl_context& ctx, bnio::ssl_context::alpn_out& out,
         std::span<const unsigned char> protocols, const std::string& protocol,
         int magic, alpn_probe* probe) -> int {
        probe->invoked = true;
        probe->seen_context = &ctx;
        probe->seen_protocol = protocol;
        probe->bound_magic = magic;
        probe->wire_match = protocols.size() == k_client_protos.size() &&
                            std::equal(protocols.begin(), protocols.end(),
                                       k_client_protos.begin());
        out.set(reinterpret_cast<const unsigned char*>(k_h2.data()),
                static_cast<unsigned char>(k_h2.size()));
        return SSL_TLSEXT_ERR_OK;
      },
      std::string(k_h2), 42, &probe);

  bnio::ssl_context client_context(bnio::ssl_context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  auto state = run_socketpair_handshake(context, client_context, server_context,
                                        k_client_protos, k_h2);

  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  EXPECT_TRUE(probe.invoked);
  // The callback must observe the very context it was installed on.
  EXPECT_EQ(probe.seen_context, &server_context);
  // protocols must carry the client's wire-format list byte for byte.
  EXPECT_TRUE(probe.wire_match);
  // Bound arguments must arrive verbatim.
  EXPECT_EQ(probe.seen_protocol, "h2");
  EXPECT_EQ(probe.bound_magic, 42);
}

// Scenario 2: native function-pointer channel with a user-owned void* arg
// selects "http/1.1" and receives the client's wire bytes.
TEST(SslAlpnTest, native_select_cb_selects_http11_and_passes_arg) {
  bnio::io_context context;
  if (!context.is_open()) {
    return;
  }

  test_certificate_files files;

  bnio::ssl_context server_context(bnio::ssl_context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());

  native_selector selector;
  selector.protocol = std::string(k_http11);
  server_context.set_alpn_select_cb(&native_alpn_select, &selector);

  bnio::ssl_context client_context(bnio::ssl_context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  auto state = run_socketpair_handshake(context, client_context, server_context,
                                        k_client_protos, k_http11);

  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  // invocations recorded through the void* arg prove the arg round-tripped
  // to the exact object the test provided.
  EXPECT_TRUE(selector.invoked);
  EXPECT_EQ(selector.received,
            std::vector<unsigned char>(k_client_protos.begin(),
                                       k_client_protos.end()));
}

// Scenario 3 (and the "never calls set" half of scenario 5): returning
// SSL_TLSEXT_ERR_NOACK without writing back leaves ALPN unselected on both
// peers while the handshake itself still succeeds.
TEST(SslAlpnTest, noack_leaves_alpn_unselected_but_handshake_succeeds) {
  bnio::io_context context;
  if (!context.is_open()) {
    return;
  }

  test_certificate_files files;

  bnio::ssl_context server_context(bnio::ssl_context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());

  server_context.set_alpn_callback([](bnio::ssl_context&,
                                      bnio::ssl_context::alpn_out&,
                                      std::span<const unsigned char>) -> int {
    return SSL_TLSEXT_ERR_NOACK;
  });

  bnio::ssl_context client_context(bnio::ssl_context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  auto state = run_socketpair_handshake(context, client_context, server_context,
                                        k_client_protos, "");

  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);
  // expect_selected_alpn already asserted empty results on both peers.
}

// Scenario 4: installing a second callback replaces the first; only the new
// closure runs and the old one is never invoked again.
TEST(SslAlpnTest, replacement_installs_latest_callback_only) {
  bnio::io_context context;
  if (!context.is_open()) {
    return;
  }

  test_certificate_files files;

  bnio::ssl_context server_context(bnio::ssl_context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());

  alpn_probe probe_a;
  alpn_probe probe_b;
  server_context.set_alpn_callback(
      [](bnio::ssl_context&, bnio::ssl_context::alpn_out& out,
         std::span<const unsigned char>, alpn_probe* probe) -> int {
        probe->invoked = true;
        out.set(reinterpret_cast<const unsigned char*>(k_h2.data()),
                static_cast<unsigned char>(k_h2.size()));
        return SSL_TLSEXT_ERR_OK;
      },
      &probe_a);
  server_context.set_alpn_callback(
      [](bnio::ssl_context&, bnio::ssl_context::alpn_out& out,
         std::span<const unsigned char>, alpn_probe* probe) -> int {
        probe->invoked = true;
        out.set(reinterpret_cast<const unsigned char*>(k_http11.data()),
                static_cast<unsigned char>(k_http11.size()));
        return SSL_TLSEXT_ERR_OK;
      },
      &probe_b);

  bnio::ssl_context client_context(bnio::ssl_context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  auto state = run_socketpair_handshake(context, client_context, server_context,
                                        k_client_protos, k_http11);

  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  EXPECT_FALSE(probe_a.invoked);
  EXPECT_TRUE(probe_b.invoked);
}

// Scenario 5 (fatal half) receiver: unlike handshake_receiver, it does not
// stop on the first error. The run() loop only exits after stop() once all
// inflight I/O is drained (kqueue should_finish()); stopping on the first
// completion would keep the peer's handshake inflight forever. Both
// completions (any value/error mix) must therefore arrive, and only then is
// stop() called.
struct alert_handshake_receiver {
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

TEST(SslAlpnTest, alert_fatal_aborts_handshake) {
  bnio::io_context context;
  if (!context.is_open()) {
    return;
  }

  test_certificate_files files;

  bnio::ssl_context server_context(bnio::ssl_context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());

  server_context.set_alpn_callback([](bnio::ssl_context&,
                                      bnio::ssl_context::alpn_out&,
                                      std::span<const unsigned char>) -> int {
    return SSL_TLSEXT_ERR_ALERT_FATAL;
  });

  bnio::ssl_context client_context(bnio::ssl_context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);

  bnio::ssl_stream client{bnio::tcp_socket(sockets[0]), client_context};
  bnio::ssl_stream server{bnio::tcp_socket(sockets[1]), server_context};

  // SSL_set_alpn_protos returns 0 on success (inverted convention).
  test_require(SSL_set_alpn_protos(
                   client.native_handle(), k_client_protos.data(),
                   static_cast<unsigned int>(k_client_protos.size())) == 0);

  auto state = std::make_shared<handshake_state>();
  auto scheduler = context.get_post_scheduler();

  auto client_operation = bexec::connect(
      client.async_handshake(scheduler, bnio::ssl_handshake_type::client),
      alert_handshake_receiver{state, &context});
  auto server_operation = bexec::connect(
      server.async_handshake(scheduler, bnio::ssl_handshake_type::server),
      alert_handshake_receiver{state, &context});
  bexec::start(client_operation);
  bexec::start(server_operation);
  context.run();

  // Both handshakes must fail with an error — no hang, no silent success.
  EXPECT_EQ(state->values, 0);
  EXPECT_EQ(state->errors, 2);
  EXPECT_EQ(state->stopped, 0);
  EXPECT_TRUE(state->error);
  // Neither peer may end up with a selected protocol.
  expect_selected_alpn(client.native_handle(), "");
  expect_selected_alpn(server.native_handle(), "");
}

// Scenario 6: lifecycle smoke — install a closure, replace it (including a
// cross-channel replacement by the native callback), then destroy the
// context. Must not crash.
TEST(SslAlpnTest, lifecycle_smoke_install_replace_destroy) {
  alpn_probe probe_a;
  alpn_probe probe_b;
  {
    bnio::ssl_context context(bnio::ssl_context_method::tls_server);
    test_require(context.valid());
    context.set_alpn_callback(
        [](bnio::ssl_context&, bnio::ssl_context::alpn_out& out,
           std::span<const unsigned char>, alpn_probe* probe) -> int {
          probe->invoked = true;
          out.set(reinterpret_cast<const unsigned char*>(k_h2.data()),
                  static_cast<unsigned char>(k_h2.size()));
          return SSL_TLSEXT_ERR_OK;
        },
        &probe_a);
    context.set_alpn_callback(
        [](bnio::ssl_context&, bnio::ssl_context::alpn_out& out,
           std::span<const unsigned char>, alpn_probe* probe) -> int {
          probe->invoked = true;
          out.set(reinterpret_cast<const unsigned char*>(k_http11.data()),
                  static_cast<unsigned char>(k_http11.size()));
          return SSL_TLSEXT_ERR_OK;
        },
        &probe_b);
    native_selector selector;
    selector.protocol = std::string(k_http11);
    context.set_alpn_select_cb(&native_alpn_select, &selector);
    // context destructor releases the owned closure and the native callback.
  }
  EXPECT_FALSE(probe_a.invoked);
  EXPECT_FALSE(probe_b.invoked);
}

}  // namespace
