/**
 * @file ssl_callback_stress_test.cpp
 * @brief Stress tests for owned SSL callback lifetimes.
 *
 * Repeatedly installs, replaces, moves, and destroys server contexts across
 * many handshakes. Assertions cover callback invocation counts and prove that
 * dynamically looked-up callbacks (ALPN, SNI servername) never run after
 * their owning context was destroyed. A copied-argument callback such as the
 * certificate callback is deliberately not exercised in the destroyed-context
 * scenario: its registration is copied into each SSL at SSL_new time, so the
 * owning context is required to outlive every stream that may invoke it. The
 * test is instrumented with AddressSanitizer by its CMake target, so a
 * use-after-free in the closure ledger or a stale native registration is
 * detected.
 */

#include <bnio/local.h>
#include <bnio/ssl.h>
#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <array>
#include <atomic>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

#include "../../support/io_context/ssl_test_support.h"

namespace {

constexpr std::array<unsigned char, 12> k_client_protos = {
    0x02, 'h', '2', 0x08, 'h', 't', 't', 'p', '/', '1', '.', '1'};

constexpr std::string_view k_h2 = "h2";
constexpr const char* k_offered_name = "stress.example.test";

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

struct stress_counters {
  std::atomic<long> first_servername_calls{0};
  std::atomic<long> replacement_servername_calls{0};
  std::atomic<long> first_cert_calls{0};
  std::atomic<long> replacement_cert_calls{0};
  std::atomic<long> alpn_calls{0};
  std::atomic<long> destroyed_context_calls{0};
};

// Runs one client/server handshake over a fresh socketpair af_unix transport.
// `before_run` runs after both streams exist but before the handshake starts,
// which lets a caller destroy the server's owning context first.
template <class BeforeRun>
std::shared_ptr<handshake_state> run_pair(bnio::ssl::context& client_context,
                                          bnio::ssl::context& server_context,
                                          BeforeRun&& before_run) {
  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);

  stream_type client{bnio::local::stream_socket(sockets[0]), client_context};
  stream_type server{bnio::local::stream_socket(sockets[1]), server_context};
  test_require(SSL_set_tlsext_host_name(client.native_handle().native_handle(),
                                        k_offered_name) == 1);
  // SSL_set_alpn_protos returns 0 on success (inverted convention).
  test_require(SSL_set_alpn_protos(
                   client.native_handle().native_handle(),
                   k_client_protos.data(),
                   static_cast<unsigned int>(k_client_protos.size())) == 0);

  std::forward<BeforeRun>(before_run)();

  bnio::io_context context;
  test_require(context.is_open());
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
  context.run();
  return state;
}

[[nodiscard]] bool succeeded(const std::shared_ptr<handshake_state>& state) {
  return state->values == 2 && state->errors == 0 && state->stopped == 0;
}

// Installs the ALPN selector, one SNI generation, and one certificate
// generation on a server context.
void install_callbacks(bnio::ssl::context& context,
                       std::atomic<long>& servername_calls,
                       std::atomic<long>& alpn_calls,
                       std::atomic<long>& cert_calls) {
  context.set_alpn_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view,
         bnio::ssl::context::alpn_out& out, std::span<const unsigned char>,
         std::atomic<long>* calls) -> int {
        calls->fetch_add(1, std::memory_order_relaxed);
        out.set(reinterpret_cast<const unsigned char*>(k_h2.data()),
                static_cast<unsigned char>(k_h2.size()));
        return SSL_TLSEXT_ERR_OK;
      },
      &alpn_calls);
  context.set_servername_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view, bnio::ssl::sni_out&,
         std::atomic<long>* calls) -> int {
        calls->fetch_add(1, std::memory_order_relaxed);
        return SSL_TLSEXT_ERR_OK;
      },
      &servername_calls);
  context.set_cert_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view, bnio::ssl::cert_out&,
         std::atomic<long>* calls) -> bnio::ssl::cert_result {
        calls->fetch_add(1, std::memory_order_relaxed);
        return bnio::ssl::cert_result::ok;
      },
      &cert_calls);
}

TEST(SslCallbackStressTest, install_replace_move_destroy_across_handshakes) {
  bnio::io_context availability_probe;
  if (!availability_probe.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  constexpr int kRounds = 16;
  stress_counters counters;

  for (int round = 0; round < kRounds; ++round) {
    // A fresh server context per round owns the callback generations.
    auto server_context = std::make_unique<bnio::ssl::context>(
        bnio::ssl::context_method::tls_server);
    load_server_identity(*server_context, files);
    install_callbacks(*server_context, counters.first_servername_calls,
                      counters.alpn_calls, counters.first_cert_calls);

    // First generation: the installed SNI, ALPN, and certificate callbacks
    // run.
    EXPECT_TRUE(succeeded(run_pair(client_context, *server_context, [] {})));

    // Replace the servername and certificate closures; only the replacement
    // generations may run now.
    server_context->set_servername_callback(
        [](bnio::ssl::context_view, bnio::ssl::stream_view, bnio::ssl::sni_out&,
           std::atomic<long>* calls) -> int {
          calls->fetch_add(1, std::memory_order_relaxed);
          return SSL_TLSEXT_ERR_OK;
        },
        &counters.replacement_servername_calls);
    server_context->set_cert_callback(
        [](bnio::ssl::context_view, bnio::ssl::stream_view,
           bnio::ssl::cert_out&,
           std::atomic<long>* calls) -> bnio::ssl::cert_result {
          calls->fetch_add(1, std::memory_order_relaxed);
          return bnio::ssl::cert_result::ok;
        },
        &counters.replacement_cert_calls);

    // Replacement generation, still on the original context.
    EXPECT_TRUE(succeeded(run_pair(client_context, *server_context, [] {})));

    // Move construction transfers the closure ledger; the native
    // registration keeps pointing at the same heap closure.
    bnio::ssl::context moved(std::move(*server_context));
    EXPECT_FALSE(server_context->valid());
    EXPECT_TRUE(moved.valid());
    EXPECT_TRUE(succeeded(run_pair(client_context, moved, [] {})));

    // Move assignment clears the target's old registrations, then adopts the
    // moved context and its callbacks.
    bnio::ssl::context target(bnio::ssl::context_method::tls_server);
    test_require(target.valid());
    target = std::move(moved);
    EXPECT_FALSE(moved.valid());
    EXPECT_TRUE(target.valid());
    EXPECT_TRUE(succeeded(run_pair(client_context, target, [] {})));

    // Destruction before the handshake: the dynamically looked-up SNI
    // registration is cleared when the owner is destroyed, so the closure is
    // never reached afterwards. Only dynamically looked-up callbacks (ALPN,
    // SNI, client_hello) may outlive their owning context; a copied-argument
    // callback such as the certificate callback is not installed here because
    // its lifetime contract requires the context to outlive the stream.
    auto doomed = std::make_unique<bnio::ssl::context>(
        bnio::ssl::context_method::tls_server);
    load_server_identity(*doomed, files);
    doomed->set_servername_callback(
        [](bnio::ssl::context_view, bnio::ssl::stream_view, bnio::ssl::sni_out&,
           std::atomic<long>* calls) -> int {
          calls->fetch_add(1, std::memory_order_relaxed);
          return SSL_TLSEXT_ERR_OK;
        },
        &counters.destroyed_context_calls);

    EXPECT_TRUE(succeeded(
        run_pair(client_context, *doomed, [&doomed] { doomed.reset(); })));
  }

  // Invocation counts: first generation once per round, replacement three
  // times per round (replace, move-construct, move-assign), ALPN four times
  // per round, and the destroyed context's callbacks never.
  EXPECT_EQ(counters.first_servername_calls.load(std::memory_order_relaxed),
            kRounds);
  EXPECT_EQ(
      counters.replacement_servername_calls.load(std::memory_order_relaxed),
      3L * kRounds);
  EXPECT_EQ(counters.first_cert_calls.load(std::memory_order_relaxed), kRounds);
  EXPECT_EQ(counters.replacement_cert_calls.load(std::memory_order_relaxed),
            3L * kRounds);
  EXPECT_EQ(counters.alpn_calls.load(std::memory_order_relaxed), 4L * kRounds);
  EXPECT_EQ(counters.destroyed_context_calls.load(std::memory_order_relaxed),
            0L);
}

}  // namespace
