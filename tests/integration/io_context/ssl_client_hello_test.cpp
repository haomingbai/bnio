/**
 * @file ssl_client_hello_test.cpp
 * @brief Integration tests for SSL client_hello callbacks.
 *
 * These tests exercise the public surface only: set_client_hello_callback,
 * the raw set_client_hello_cb setter, the alert_out proxy, and the 3-argument
 * async_handshake retry factory. TLS 1.3 is forced on both peers because the
 * client_hello callback only runs for TLS 1.3 servers.
 *
 * The tests are written before the P1 implementation, so they are expected to
 * fail against the skeleton.
 */

#include <bnio/local.h>
#include <bnio/ssl.h>
#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <bexec/completion_signatures.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "../../support/io_context/ssl_test_support.h"

namespace {

constexpr const char* k_offered_name = "client-hello.example.test";

using stream_type = bnio::ssl::tcp::stream<bnio::local::stream_socket>;
using post_scheduler_type =
    decltype(std::declval<bnio::io_context&>().get_post_scheduler());

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

// Restricts a context to TLS 1.3; the client_hello callback only runs for
// TLS 1.3 servers.
void force_tls13(bnio::ssl::context& context) {
  test_require(!context.set_min_proto_version(TLS1_3_VERSION));
  test_require(!context.set_max_proto_version(TLS1_3_VERSION));
}

// Extracts the SNI host name from the ClientHello carried by the connection.
//
// OpenSSL invokes the client_hello callback before the servername extension
// finalizer, so SSL_get_servername() still returns null there; the standard
// way to observe the name is the raw extension accessor.
std::string client_hello_servername(bnio::ssl::stream_view ssl) {
  const unsigned char* ext = nullptr;
  std::size_t extlen = 0;
  if (SSL_client_hello_get0_ext(ssl.native_handle(), TLSEXT_TYPE_server_name,
                                &ext, &extlen) == 0) {
    return {};
  }
  // server_name extension: 2-byte list length, then one entry with a 1-byte
  // name type and a 2-byte host name length.
  if (extlen < 5) {
    return {};
  }
  const std::size_t name_len = (static_cast<std::size_t>(ext[3]) << 8) | ext[4];
  if (extlen < 5 + name_len) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(ext + 5), name_len);
}

// Records what the high-level (set_client_hello_callback) callback observed.
struct client_hello_probe {
  bool invoked = false;
  std::string seen_servername;
  bnio::ssl::context_view seen_context;
  bool connection_matches_context = false;
  bool is_server_connection = false;
  int bound_magic = 0;
  unsigned calls = 0;
};

// Captures alert records received by the client so tests can prove which
// alert the peer actually sent.
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

// Receiver that waits for both terminal signals before stopping the context;
// a fatal server callback makes both handshakes fail.
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

// Socketpair-backed client/server pair with SNI offered by the client.
struct stream_pair {
  int sockets[2] = {-1, -1};
  std::unique_ptr<stream_type> client;
  std::unique_ptr<stream_type> server;

  stream_pair(bnio::ssl::context& client_context,
              bnio::ssl::context& server_context, const char* offered_name) {
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
  }
};

// Drives a successful handshake between two streams of the caller's pair.
std::shared_ptr<handshake_state> handshake_pair(bnio::io_context& context,
                                                stream_pair& pair) {
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
  return state;
}

// Sender used by callback-driven retry factories: callback pauses resume the
// handshake immediately (OpenSSL re-invokes the callback on the next attempt)
// while transport pauses delegate to the public wait helper. The factory
// return type is fixed, so the choice is made at start() time.
template <class Scheduler, class NextLayer>
class callback_resume_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                   bexec::set_stopped_t()>;
  using wait_sender_type = decltype(bnio::ssl::make_handshake_wait_sender(
      std::declval<Scheduler>(),
      std::declval<bnio::ssl::tcp::stream<NextLayer>&>(),
      std::declval<std::error_code>()));

  callback_resume_sender(Scheduler scheduler,
                         bnio::ssl::tcp::stream<NextLayer>& stream,
                         std::error_code pause)
      : scheduler_(std::move(scheduler)), stream_(&stream), pause_(pause) {}

  template <class Receiver>
  class operation {
   public:
    using wait_operation_type = decltype(bexec::connect(
        std::declval<wait_sender_type>(), std::declval<Receiver>()));

    operation(Scheduler scheduler, bnio::ssl::tcp::stream<NextLayer>& stream,
              std::error_code pause, Receiver receiver)
        : scheduler_(std::move(scheduler)),
          stream_(&stream),
          pause_(pause),
          receiver_(std::move(receiver)) {}

    operation(const operation&) = delete;
    operation& operator=(const operation&) = delete;

    ~operation() noexcept {
      if (wait_started_) {
        storage_.wait_operation.~wait_operation_type();
      }
    }

    void start() noexcept {
      const auto reason = bnio::ssl::handshake_pause_reason_of(pause_);
      if (reason == bnio::ssl::handshake_pause_reason::client_hello_cb ||
          reason == bnio::ssl::handshake_pause_reason::x509_lookup) {
        // Resume immediately: OpenSSL re-invokes the callback on the next
        // attempt, and the data it needs is already in the read BIO.
        bexec::set_value(std::move(receiver_), std::error_code{});
        return;
      }
      auto wait_sender =
          bnio::ssl::make_handshake_wait_sender(scheduler_, *stream_, pause_);
      ::new (static_cast<void*>(&storage_.wait_operation)) wait_operation_type(
          bexec::connect(std::move(wait_sender), std::move(receiver_)));
      wait_started_ = true;
      bexec::start(storage_.wait_operation);
    }

   private:
    union storage_type {
      wait_operation_type wait_operation;
      storage_type() noexcept {}
      ~storage_type() noexcept {}
    };

    Scheduler scheduler_;
    bnio::ssl::tcp::stream<NextLayer>* stream_;
    std::error_code pause_;
    Receiver receiver_;
    storage_type storage_{};
    bool wait_started_ = false;
  };

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return operation<std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), *stream_, pause_, std::move(receiver));
  }

  template <class Receiver>
    requires std::copy_constructible<Scheduler>
  auto connect(Receiver receiver) const& {
    return operation<std::remove_cvref_t<Receiver>>(
        scheduler_, *stream_, pause_, std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  bnio::ssl::tcp::stream<NextLayer>* stream_;
  std::error_code pause_;
};

struct retry_observation {
  unsigned calls = 0;
  unsigned client_hello_pauses = 0;
  std::vector<bnio::ssl::handshake_pause_reason> reasons;
};

struct client_hello_retry_factory {
  post_scheduler_type scheduler;
  stream_type* stream = nullptr;
  std::shared_ptr<retry_observation> observation;

  auto operator()(const std::error_code& ec) const {
    ++observation->calls;
    const auto reason = bnio::ssl::handshake_pause_reason_of(ec);
    observation->reasons.push_back(reason);
    if (reason == bnio::ssl::handshake_pause_reason::client_hello_cb) {
      ++observation->client_hello_pauses;
    }
    return callback_resume_sender<post_scheduler_type,
                                  bnio::local::stream_socket>(scheduler,
                                                              *stream, ec);
  }
};

// Scenario 1: the callback observes the ClientHello servername, its context,
// the server connection, and its bound arguments, then returns success.
TEST(SslClientHelloTest, callback_observes_servername_and_returns_success) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  load_server_identity(server_context, files);
  force_tls13(server_context);

  client_hello_probe probe;
  server_context.set_client_hello_callback(
      [](bnio::ssl::context_view ctx, bnio::ssl::stream_view ssl,
         bnio::ssl::alert_out& /*out*/, int magic,
         client_hello_probe* observed) -> bnio::ssl::client_hello_result {
        observed->invoked = true;
        ++observed->calls;
        observed->seen_context = ctx;
        observed->seen_servername = client_hello_servername(ssl);
        observed->connection_matches_context =
            SSL_get_SSL_CTX(ssl.native_handle()) == ctx.native_handle();
        observed->is_server_connection =
            SSL_is_server(ssl.native_handle()) != 0;
        observed->bound_magic = magic;
        return bnio::ssl::client_hello_result::success;
      },
      17, &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  force_tls13(client_context);
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  stream_pair pair(client_context, server_context, k_offered_name);
  auto state = handshake_pair(context, pair);
  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(probe.calls, 1u);
  EXPECT_EQ(probe.seen_servername, k_offered_name);
  EXPECT_EQ(probe.seen_context.native_handle(), server_context.native_handle());
  EXPECT_TRUE(probe.connection_matches_context);
  EXPECT_TRUE(probe.is_server_connection);
  EXPECT_EQ(probe.bound_magic, 17);
}

// Scenario 2: retry once, then success, through the 3-argument factory. The
// factory must see handshake_pause_reason::client_hello_cb and the handshake
// must complete successfully.
TEST(SslClientHelloTest,
     retry_then_success_through_factory_reports_client_hello_pause) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  load_server_identity(server_context, files);
  force_tls13(server_context);

  client_hello_probe probe;
  server_context.set_client_hello_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view ssl,
         bnio::ssl::alert_out&,
         client_hello_probe* observed) -> bnio::ssl::client_hello_result {
        observed->invoked = true;
        ++observed->calls;
        observed->seen_servername = client_hello_servername(ssl);
        if (observed->calls == 1) {
          return bnio::ssl::client_hello_result::retry;
        }
        return bnio::ssl::client_hello_result::success;
      },
      &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  force_tls13(client_context);
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  stream_pair pair(client_context, server_context, k_offered_name);
  auto observation = std::make_shared<retry_observation>();
  auto state = std::make_shared<handshake_state>();
  auto scheduler = context.get_post_scheduler();

  auto client_operation =
      bexec::connect(pair.client->async_handshake(
                         scheduler, bnio::ssl::handshake_type::client),
                     handshake_receiver{state, &context});
  auto server_sender = pair.server->async_handshake(
      scheduler, bnio::ssl::handshake_type::server,
      client_hello_retry_factory{scheduler, pair.server.get(), observation});
  auto server_operation = bexec::connect(std::move(server_sender),
                                         handshake_receiver{state, &context});
  bexec::start(client_operation);
  bexec::start(server_operation);
  context.run();

  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(probe.calls, 2u);
  EXPECT_EQ(probe.seen_servername, k_offered_name);
  ASSERT_GE(observation->calls, 2u);
  EXPECT_EQ(observation->client_hello_pauses, 1u);
  for (const auto reason : observation->reasons) {
    EXPECT_NE(reason, bnio::ssl::handshake_pause_reason::none);
  }
}

// Scenario 3: an error result fails the handshake, and the alert recorded
// through alert_out reaches the peer.
TEST(SslClientHelloTest, error_fails_handshake_and_alert_reaches_peer) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  load_server_identity(server_context, files);
  force_tls13(server_context);

  client_hello_probe probe;
  server_context.set_client_hello_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view,
         bnio::ssl::alert_out& out,
         client_hello_probe* observed) -> bnio::ssl::client_hello_result {
        observed->invoked = true;
        out.set(SSL_AD_ACCESS_DENIED);
        return bnio::ssl::client_hello_result::error;
      },
      &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  force_tls13(client_context);
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  alert_capture capture;
  SSL_CTX_set_msg_callback(client_context.native_handle(),
                           &alert_message_callback);
  SSL_CTX_set_msg_callback_arg(client_context.native_handle(), &capture);

  stream_pair pair(client_context, server_context, k_offered_name);
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

  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(state->values, 0);
  EXPECT_EQ(state->errors, 2);
  EXPECT_EQ(state->stopped, 0);
  EXPECT_TRUE(state->error);
  EXPECT_TRUE(alerts_contain(capture, SSL_AD_ACCESS_DENIED))
      << "the client did not receive the alert recorded by the callback";
}

// Scenario 4: a throwing callback fails closed with SSL_AD_INTERNAL_ERROR; no
// exception crosses the OpenSSL C boundary.
TEST(SslClientHelloTest, throwing_callback_fails_closed) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  load_server_identity(server_context, files);
  force_tls13(server_context);

  client_hello_probe probe;
  server_context.set_client_hello_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view, bnio::ssl::alert_out&,
         client_hello_probe* observed) -> bnio::ssl::client_hello_result {
        observed->invoked = true;
        throw std::runtime_error("client_hello callback failure");
      },
      &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  force_tls13(client_context);
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  alert_capture capture;
  SSL_CTX_set_msg_callback(client_context.native_handle(),
                           &alert_message_callback);
  SSL_CTX_set_msg_callback_arg(client_context.native_handle(), &capture);

  stream_pair pair(client_context, server_context, k_offered_name);
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

  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(state->values, 0);
  EXPECT_EQ(state->errors, 2);
  EXPECT_EQ(state->stopped, 0);
  EXPECT_TRUE(state->error);
  EXPECT_TRUE(alerts_contain(capture, SSL_AD_INTERNAL_ERROR))
      << "the fail-closed alert did not reach the peer";
}

// Scenario 5: the raw set_client_hello_cb setter round-trips the function
// pointer and void* argument.
TEST(SslClientHelloTest, native_client_hello_callback_round_trips) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  load_server_identity(server_context, files);
  force_tls13(server_context);

  client_hello_probe probe;
  server_context.set_client_hello_cb(
      [](SSL* ssl, int* /*al*/, void* arg) -> int {
        auto* observed = static_cast<client_hello_probe*>(arg);
        observed->invoked = true;
        ++observed->calls;
        observed->seen_servername =
            client_hello_servername(bnio::ssl::stream_view(ssl));
        return SSL_CLIENT_HELLO_SUCCESS;
      },
      &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  force_tls13(client_context);
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  stream_pair pair(client_context, server_context, k_offered_name);
  auto state = handshake_pair(context, pair);
  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(probe.calls, 1u);
  EXPECT_EQ(probe.seen_servername, k_offered_name);
}

}  // namespace
