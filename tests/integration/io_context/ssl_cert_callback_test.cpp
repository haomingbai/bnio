/**
 * @file ssl_cert_callback_test.cpp
 * @brief Integration tests for SSL per-connection certificate callbacks.
 *
 * These tests exercise the public surface only: set_cert_callback, the raw
 * set_cert_cb setter, the cert_out proxy, the in-memory certificate
 * installation on context_view, and the 3-argument async_handshake retry
 * factory. The tests are written before the P2 implementation, so they are
 * expected to fail against the skeleton.
 */

#include <bnio/local.h>
#include <bnio/ssl.h>
#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "../../support/io_context/ssl_test_support.h"

namespace {

using stream_type = bnio::ssl::tcp::stream<bnio::local::stream_socket>;
using post_scheduler_type =
    decltype(std::declval<bnio::io_context&>().get_post_scheduler());

// Owns one parsed copy of the embedded PEM fixture.
struct identity_bundle {
  std::unique_ptr<X509, decltype(&X509_free)> certificate{nullptr, &X509_free};
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> private_key{
      nullptr, &EVP_PKEY_free};
};

identity_bundle parse_test_identity() {
  identity_bundle bundle;
  BIO* certificate_bio = BIO_new_mem_buf(
      k_test_certificate.data(), static_cast<int>(k_test_certificate.size()));
  test_require(certificate_bio != nullptr);
  bundle.certificate.reset(
      PEM_read_bio_X509(certificate_bio, nullptr, nullptr, nullptr));
  BIO_free(certificate_bio);
  test_require(bundle.certificate != nullptr);

  BIO* key_bio = BIO_new_mem_buf(k_test_private_key.data(),
                                 static_cast<int>(k_test_private_key.size()));
  test_require(key_bio != nullptr);
  bundle.private_key.reset(
      PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr));
  BIO_free(key_bio);
  test_require(bundle.private_key != nullptr);
  return bundle;
}

// Loads the shared certificate/key files into a server context.
void load_server_identity(bnio::ssl::context& context,
                          const test_certificate_files& files) {
  test_require(context.valid());
  test_require(
      !context.use_certificate_chain_file(files.certificate.string().c_str()));
  test_require(
      !context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!context.check_private_key());
}

// Records what the high-level (set_cert_callback) callback observed.
struct cert_probe {
  bool invoked = false;
  unsigned calls = 0;
  bnio::ssl::context_view seen_context;
  SSL_CTX* expected_context_handle = nullptr;
  bool connection_matches_context = false;
  bool is_server_connection = false;
  int bound_magic = 0;
  std::error_code certificate_error;
  std::error_code private_key_error;
};

// Receiver that waits for both terminal signals before stopping the context;
// a failing certificate callback makes both handshakes fail.
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

// Socketpair-backed client/server pair.
struct stream_pair {
  int sockets[2] = {-1, -1};
  std::unique_ptr<stream_type> client;
  std::unique_ptr<stream_type> server;

  stream_pair(bnio::ssl::context& client_context,
              bnio::ssl::context& server_context) {
    test_require(
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    client = std::make_unique<stream_type>(
        bnio::local::stream_socket(sockets[0]), client_context);
    server = std::make_unique<stream_type>(
        bnio::local::stream_socket(sockets[1]), server_context);
  }
};

// Drives a successful handshake between the two streams of a pair.
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
// handshake immediately while transport pauses delegate to the public wait
// helper. The factory return type is fixed, so the choice is made at start().
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
  unsigned x509_lookup_pauses = 0;
  std::vector<bnio::ssl::handshake_pause_reason> reasons;
};

struct cert_retry_factory {
  post_scheduler_type scheduler;
  stream_type* stream = nullptr;
  std::shared_ptr<retry_observation> observation;

  auto operator()(const std::error_code& ec) const {
    ++observation->calls;
    const auto reason = bnio::ssl::handshake_pause_reason_of(ec);
    observation->reasons.push_back(reason);
    if (reason == bnio::ssl::handshake_pause_reason::x509_lookup) {
      ++observation->x509_lookup_pauses;
    }
    return callback_resume_sender<post_scheduler_type,
                                  bnio::local::stream_socket>(scheduler,
                                                              *stream, ec);
  }
};

// Scenario 1: a server context without file certificates installs a
// certificate and key parsed from the embedded PEM fixture through cert_out,
// and the handshake succeeds.
TEST(SslCertCallbackTest, use_certificate_and_private_key_install_identity) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  identity_bundle identity = parse_test_identity();

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  test_require(server_context.valid());

  cert_probe probe;
  server_context.set_cert_callback(
      [](bnio::ssl::context_view ctx, bnio::ssl::stream_view ssl,
         bnio::ssl::cert_out& out, int magic, X509* certificate,
         EVP_PKEY* private_key,
         cert_probe* observed) -> bnio::ssl::cert_result {
        observed->invoked = true;
        ++observed->calls;
        observed->seen_context = ctx;
        observed->connection_matches_context =
            SSL_get_SSL_CTX(ssl.native_handle()) == ctx.native_handle();
        observed->is_server_connection =
            SSL_is_server(ssl.native_handle()) != 0;
        observed->bound_magic = magic;
        observed->certificate_error = out.use_certificate(certificate);
        observed->private_key_error = out.use_private_key(private_key);
        return bnio::ssl::cert_result::ok;
      },
      23, identity.certificate.get(), identity.private_key.get(), &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  stream_pair pair(client_context, server_context);
  auto state = handshake_pair(context, pair);
  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(probe.calls, 1u);
  EXPECT_EQ(probe.seen_context.native_handle(), server_context.native_handle());
  EXPECT_TRUE(probe.connection_matches_context);
  EXPECT_TRUE(probe.is_server_connection);
  EXPECT_EQ(probe.bound_magic, 23);
  EXPECT_FALSE(probe.certificate_error);
  EXPECT_FALSE(probe.private_key_error);
}

// Scenario 2: retry once, then ok, through the 3-argument factory; the
// factory must see handshake_pause_reason::x509_lookup.
TEST(SslCertCallbackTest, retry_then_ok_through_factory_reports_x509_lookup) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  identity_bundle identity = parse_test_identity();

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  test_require(server_context.valid());

  cert_probe probe;
  server_context.set_cert_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view,
         bnio::ssl::cert_out& out, X509* certificate, EVP_PKEY* private_key,
         cert_probe* observed) -> bnio::ssl::cert_result {
        observed->invoked = true;
        ++observed->calls;
        if (observed->calls == 1) {
          return bnio::ssl::cert_result::retry;
        }
        observed->certificate_error = out.use_certificate(certificate);
        observed->private_key_error = out.use_private_key(private_key);
        return bnio::ssl::cert_result::ok;
      },
      identity.certificate.get(), identity.private_key.get(), &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  stream_pair pair(client_context, server_context);
  auto observation = std::make_shared<retry_observation>();
  auto state = std::make_shared<handshake_state>();
  auto scheduler = context.get_post_scheduler();

  auto client_operation =
      bexec::connect(pair.client->async_handshake(
                         scheduler, bnio::ssl::handshake_type::client),
                     handshake_receiver{state, &context});
  auto server_sender = pair.server->async_handshake(
      scheduler, bnio::ssl::handshake_type::server,
      cert_retry_factory{scheduler, pair.server.get(), observation});
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
  EXPECT_FALSE(probe.certificate_error);
  EXPECT_FALSE(probe.private_key_error);
  ASSERT_GE(observation->calls, 2u);
  EXPECT_EQ(observation->x509_lookup_pauses, 1u);
  for (const auto reason : observation->reasons) {
    EXPECT_NE(reason, bnio::ssl::handshake_pause_reason::none);
  }
}

// Scenario 3: a fail result aborts the handshake.
TEST(SslCertCallbackTest, fail_aborts_handshake) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  test_require(server_context.valid());

  cert_probe probe;
  server_context.set_cert_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view, bnio::ssl::cert_out&,
         cert_probe* observed) -> bnio::ssl::cert_result {
        observed->invoked = true;
        return bnio::ssl::cert_result::fail;
      },
      &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  stream_pair pair(client_context, server_context);
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
}

// Scenario 4: a throwing callback fails closed by returning 0; the handshake
// aborts and no exception crosses the OpenSSL C boundary.
TEST(SslCertCallbackTest, throwing_callback_fails_closed) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  test_require(server_context.valid());

  cert_probe probe;
  server_context.set_cert_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view, bnio::ssl::cert_out&,
         cert_probe* observed) -> bnio::ssl::cert_result {
        observed->invoked = true;
        throw std::runtime_error("cert callback failure");
      },
      &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  stream_pair pair(client_context, server_context);
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
}

// Scenario 4b: replacing the certificate callback before any stream exists
// means the latest generation is the one OpenSSL copies into the connection.
// No stream from the previous generation is alive, so this is the supported
// replacement order.
TEST(SslCertCallbackTest,
     replacing_callback_before_stream_creation_runs_latest_generation) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  identity_bundle identity = parse_test_identity();

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  test_require(server_context.valid());

  cert_probe first_probe;
  server_context.set_cert_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view, bnio::ssl::cert_out&,
         cert_probe* observed) -> bnio::ssl::cert_result {
        observed->invoked = true;
        return bnio::ssl::cert_result::fail;
      },
      &first_probe);

  cert_probe second_probe;
  server_context.set_cert_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view,
         bnio::ssl::cert_out& out, X509* certificate, EVP_PKEY* private_key,
         cert_probe* observed) -> bnio::ssl::cert_result {
        observed->invoked = true;
        ++observed->calls;
        observed->certificate_error = out.use_certificate(certificate);
        observed->private_key_error = out.use_private_key(private_key);
        return bnio::ssl::cert_result::ok;
      },
      identity.certificate.get(), identity.private_key.get(), &second_probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  // Both SSL objects are created after the replacement and copy only the
  // latest generation's registration.
  stream_pair pair(client_context, server_context);
  auto state = handshake_pair(context, pair);
  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  EXPECT_FALSE(first_probe.invoked);
  EXPECT_TRUE(second_probe.invoked);
  EXPECT_EQ(second_probe.calls, 1u);
  EXPECT_FALSE(second_probe.certificate_error);
  EXPECT_FALSE(second_probe.private_key_error);
}

// Scenario 4c: a direct stream.set_context switch copies the target context's
// certificate callback into the connection. Keeping the target context alive
// for the whole stream lifetime lets that callback run and supply the
// identity. This is the supported context-switch contract.
TEST(SslCertCallbackTest,
     context_switch_to_live_target_runs_target_cert_callback) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  identity_bundle identity = parse_test_identity();

  bnio::ssl::context initial_context(bnio::ssl::context_method::tls_server);
  test_require(initial_context.valid());

  // Declared before the stream pair so it outlives the connection whose
  // registration OpenSSL copies from it.
  bnio::ssl::context target_context(bnio::ssl::context_method::tls_server);
  test_require(target_context.valid());

  cert_probe probe;
  target_context.set_cert_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view,
         bnio::ssl::cert_out& out, X509* certificate, EVP_PKEY* private_key,
         cert_probe* observed) -> bnio::ssl::cert_result {
        observed->invoked = true;
        ++observed->calls;
        observed->certificate_error = out.use_certificate(certificate);
        observed->private_key_error = out.use_private_key(private_key);
        return bnio::ssl::cert_result::ok;
      },
      identity.certificate.get(), identity.private_key.get(), &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  stream_pair pair(client_context, initial_context);
  EXPECT_FALSE(pair.server->set_context(target_context));

  auto state = handshake_pair(context, pair);
  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(probe.calls, 1u);
  EXPECT_FALSE(probe.certificate_error);
  EXPECT_FALSE(probe.private_key_error);
}

// Scenario 5: context_view::use_certificate / use_private_key install an
// identity from memory, and a handshake without any file certificate works.
TEST(SslCertCallbackTest,
     context_view_use_certificate_and_private_key_install_from_memory) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  identity_bundle identity = parse_test_identity();

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  test_require(server_context.valid());

  const std::error_code certificate_error =
      server_context.view().use_certificate(identity.certificate.get());
  const std::error_code private_key_error =
      server_context.view().use_private_key(identity.private_key.get());
  EXPECT_FALSE(certificate_error);
  EXPECT_FALSE(private_key_error);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  stream_pair pair(client_context, server_context);
  auto state = handshake_pair(context, pair);
  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);
}

// Scenario 6: the raw set_cert_cb setter round-trips the function pointer and
// void* argument.
TEST(SslCertCallbackTest, native_cert_callback_round_trips) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  test_certificate_files files;

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  load_server_identity(server_context, files);

  cert_probe probe;
  probe.expected_context_handle = server_context.native_handle();
  server_context.set_cert_cb(
      [](SSL* ssl, void* arg) -> int {
        auto* observed = static_cast<cert_probe*>(arg);
        observed->invoked = true;
        ++observed->calls;
        observed->connection_matches_context =
            SSL_get_SSL_CTX(ssl) == observed->expected_context_handle;
        observed->is_server_connection = SSL_is_server(ssl) != 0;
        // The context supplied a usable identity, so this callback only
        // observes the invocation and lets OpenSSL continue.
        return 1;
      },
      &probe);

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  stream_pair pair(client_context, server_context);
  auto state = handshake_pair(context, pair);
  ASSERT_EQ(state->values, 2);
  EXPECT_EQ(state->errors, 0);
  EXPECT_EQ(state->stopped, 0);

  EXPECT_TRUE(probe.invoked);
  EXPECT_EQ(probe.calls, 1u);
  EXPECT_TRUE(probe.connection_matches_context);
  EXPECT_TRUE(probe.is_server_connection);
}

}  // namespace
