/**
 * @file ssl_handshake_retry_test.cpp
 * @brief Integration tests for the factory-driven SSL handshake retry API.
 *
 * These tests exercise the public retry surface only: the 3-argument
 * async_handshake, bnio::ssl::make_handshake_wait_sender, and the public
 * handshake-pause error category. No bnio detail header is included.
 */

#include <bnio/local.h>
#include <bnio/ssl.h>
#include <gtest/gtest.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <bexec/just.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <bexec/stop_token.hpp>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "../../support/io_context/ssl_test_support.h"

namespace {

using client_stream_type = bnio::ssl::tcp::stream<bnio::local::stream_socket>;
using post_scheduler_type =
    decltype(std::declval<bnio::io_context&>().get_post_scheduler());

// The pause-reason enum values are part of the public contract: 1..7 in the
// documented order, deliberately unrelated to the SSL_ERROR_* numbers.
static_assert(static_cast<int>(bnio::ssl::handshake_pause_reason::none) == 0);
static_assert(static_cast<int>(bnio::ssl::handshake_pause_reason::want_read) ==
              1);
static_assert(static_cast<int>(bnio::ssl::handshake_pause_reason::want_write) ==
              2);
static_assert(
    static_cast<int>(bnio::ssl::handshake_pause_reason::x509_lookup) == 3);
static_assert(static_cast<int>(bnio::ssl::handshake_pause_reason::async) == 4);
static_assert(static_cast<int>(bnio::ssl::handshake_pause_reason::async_job) ==
              5);
static_assert(
    static_cast<int>(bnio::ssl::handshake_pause_reason::client_hello_cb) == 6);
static_assert(
    static_cast<int>(bnio::ssl::handshake_pause_reason::retry_verify) == 7);

[[nodiscard]] bool is_table_pause_reason(
    bnio::ssl::handshake_pause_reason reason) noexcept {
  const int value = static_cast<int>(reason);
  return value >=
             static_cast<int>(bnio::ssl::handshake_pause_reason::want_read) &&
         value <=
             static_cast<int>(bnio::ssl::handshake_pause_reason::retry_verify);
}

// Every pause code handed to a user factory must satisfy the public pause
// contract and carry one of the reasons the retry table classifies.
void expect_pause_code(const std::error_code& ec) {
  EXPECT_TRUE(bnio::ssl::is_handshake_pause(ec))
      << "non-pause error reached the retry factory: " << ec.message();
  EXPECT_EQ(ec.category(), bnio::ssl::handshake_pause_category());
  EXPECT_EQ(std::string(ec.category().name()), "bnio.ssl.handshake_pause");
  const auto reason = bnio::ssl::handshake_pause_reason_of(ec);
  EXPECT_TRUE(is_table_pause_reason(reason))
      << "pause reason " << static_cast<int>(reason)
      << " is not a table reason";
}

struct retry_record {
  std::vector<std::error_code> pauses;
  unsigned calls = 0;
};

// Records every factory invocation and returns the public wait helper, which
// is exactly the intended user composition for a real handshake.
struct recording_wait_factory {
  post_scheduler_type scheduler;
  client_stream_type* stream = nullptr;
  std::shared_ptr<retry_record> record;
  std::atomic<bool>* paused = nullptr;

  auto operator()(const std::error_code& ec) const {
    ++record->calls;
    record->pauses.push_back(ec);
    if (paused != nullptr) {
      paused->store(true, std::memory_order_release);
    }
    return bnio::ssl::make_handshake_wait_sender(scheduler, *stream, ec);
  }
};

// Terminal-error factory: a fixed sender type that completes synchronously
// with a caller-chosen non-pause error.
struct terminal_error_factory {
  std::shared_ptr<retry_record> record;
  std::error_code terminal;

  auto operator()(const std::error_code& ec) const {
    ++record->calls;
    record->pauses.push_back(ec);
    return bexec::just(terminal);
  }
};

// Checks, at the first factory invocation, that the pause's output flush had
// already reached the transport: the write BIO is empty and the peer socket
// has bytes available. Completes terminally so the test ends immediately.
struct ordering_check_factory {
  post_scheduler_type scheduler;
  client_stream_type* stream = nullptr;
  int peer_fd = -1;
  std::shared_ptr<retry_record> record;
  std::shared_ptr<std::atomic<bool>> write_bio_drained;
  std::shared_ptr<std::atomic<bool>> peer_received;

  auto operator()(const std::error_code& ec) const {
    ++record->calls;
    record->pauses.push_back(ec);
    if (record->calls == 1) {
      write_bio_drained->store(
          BIO_ctrl_pending(stream->native_write_bio()) == 0,
          std::memory_order_release);
      char byte = 0;
      const ssize_t available =
          ::recv(peer_fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
      peer_received->store(available > 0, std::memory_order_release);
    }
    return bexec::just(std::make_error_code(std::errc::permission_denied));
  }
};

// Never-invoked factory used by the invalid-stream case; returning a valid
// sender type keeps the concept satisfied while the count stays at zero.
struct counting_just_factory {
  std::shared_ptr<std::atomic<unsigned>> calls;
  std::error_code terminal;

  auto operator()(const std::error_code&) const {
    calls->fetch_add(1, std::memory_order_relaxed);
    return bexec::just(terminal);
  }
};

struct client_observation {
  unsigned values = 0;
  unsigned errors = 0;
  unsigned stopped = 0;
  std::error_code last_error;
  bool saw_pause_code = false;
};

// Public receiver that records each terminal signal and stops the context so
// a single-operation test always terminates.
struct observing_handshake_receiver {
  std::shared_ptr<client_observation> observation;
  bnio::io_context* context = nullptr;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      ++observation->errors;
      observation->last_error = ec;
      if (bnio::ssl::is_handshake_pause(ec)) {
        observation->saw_pause_code = true;
      }
    } else {
      ++observation->values;
    }
    complete();
  }

  void set_stopped() noexcept {
    ++observation->stopped;
    complete();
  }

 private:
  void complete() noexcept {
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct stop_observing_handshake_receiver : observing_handshake_receiver {
  stop_env env;

  [[nodiscard]] stop_env get_env() const noexcept { return env; }
};

TEST(SslHandshakeRetryTest, pause_error_public_contract) {
  const auto& category = bnio::ssl::handshake_pause_category();
  EXPECT_EQ(std::string(category.name()), "bnio.ssl.handshake_pause");

  const std::error_code no_pause = bnio::ssl::make_handshake_pause_error(
      bnio::ssl::handshake_pause_reason::none);
  EXPECT_EQ(no_pause, std::error_code{});
  EXPECT_FALSE(no_pause);
  EXPECT_FALSE(bnio::ssl::is_handshake_pause(no_pause));
  EXPECT_FALSE(std::string(no_pause.message()).empty());
  EXPECT_EQ(bnio::ssl::handshake_pause_reason_of(no_pause),
            bnio::ssl::handshake_pause_reason::none);

  const bnio::ssl::handshake_pause_reason reasons[] = {
      bnio::ssl::handshake_pause_reason::want_read,
      bnio::ssl::handshake_pause_reason::want_write,
      bnio::ssl::handshake_pause_reason::x509_lookup,
      bnio::ssl::handshake_pause_reason::async,
      bnio::ssl::handshake_pause_reason::async_job,
      bnio::ssl::handshake_pause_reason::client_hello_cb,
      bnio::ssl::handshake_pause_reason::retry_verify,
  };
  for (const auto reason : reasons) {
    const std::error_code ec = bnio::ssl::make_handshake_pause_error(reason);
    EXPECT_TRUE(ec);
    EXPECT_TRUE(bnio::ssl::is_handshake_pause(ec));
    EXPECT_EQ(ec.category(), category);
    EXPECT_EQ(bnio::ssl::handshake_pause_reason_of(ec), reason);
    EXPECT_FALSE(std::string(ec.message()).empty());
    EXPECT_EQ(bnio::ssl::make_handshake_pause_error(reason).message(),
              ec.message());
  }

  // A non-zero value inside the pause category is still pause-category but
  // has no known reason; the classifier must not invent one.
  const std::error_code unknown{77, category};
  EXPECT_TRUE(bnio::ssl::is_handshake_pause(unknown));
  EXPECT_EQ(bnio::ssl::handshake_pause_reason_of(unknown),
            bnio::ssl::handshake_pause_reason::none);
  EXPECT_FALSE(std::string(unknown.message()).empty());
  EXPECT_FALSE(std::string(std::error_code(0, category).message()).empty());

  // Codes from another category are never pauses, whatever their value.
  EXPECT_FALSE(bnio::ssl::is_handshake_pause(
      std::make_error_code(std::errc::permission_denied)));
  EXPECT_EQ(bnio::ssl::handshake_pause_reason_of(
                std::make_error_code(std::errc::permission_denied)),
            bnio::ssl::handshake_pause_reason::none);
}

TEST(SslHandshakeRetryTest, socketpair_handshake_drives_factory_waits) {
  test_certificate_files files;

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);

  client_stream_type client{bnio::local::stream_socket(sockets[0]),
                            client_context};
  client_stream_type server{bnio::local::stream_socket(sockets[1]),
                            server_context};

  auto record = std::make_shared<retry_record>();
  auto state = std::make_shared<handshake_state>();
  handshake_receiver client_receiver{state, &context};
  handshake_receiver server_receiver{state, &context};

  auto client_sender = client.async_handshake(
      scheduler, bnio::ssl::handshake_type::client,
      recording_wait_factory{scheduler, &client, record, nullptr});
  auto server_sender =
      server.async_handshake(scheduler, bnio::ssl::handshake_type::server);

  auto client_operation =
      bexec::connect(std::move(client_sender), std::move(client_receiver));
  auto server_operation =
      bexec::connect(std::move(server_sender), std::move(server_receiver));

  bexec::start(client_operation);
  bexec::start(server_operation);
  context.run();

  EXPECT_EQ(state->values, 2u);
  EXPECT_EQ(state->errors, 0u);
  EXPECT_EQ(state->stopped, 0u);
  EXPECT_FALSE(state->error);

  ASSERT_GE(record->calls, 1u);
  ASSERT_EQ(record->pauses.size(), record->calls);
  for (const auto& ec : record->pauses) {
    expect_pause_code(ec);
  }
}

TEST(SslHandshakeRetryTest, invalid_stream_skips_factory_entirely) {
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  test_require(ssl_context.valid());
  bnio::ssl::tcp::stream source{bnio::tcp_socket(-1), ssl_context};
  bnio::ssl::tcp::stream owner{std::move(source)};
  ASSERT_FALSE(source.valid());
  ASSERT_TRUE(owner.valid());

  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  auto observation = std::make_shared<client_observation>();

  auto sender = source.async_handshake(
      scheduler, bnio::ssl::handshake_type::client,
      counting_just_factory{calls, bnio::ssl::base::empty_error_code});
  auto operation = bexec::connect(
      std::move(sender), observing_handshake_receiver{observation, &context});
  bexec::start(operation);
  context.run();

  EXPECT_EQ(observation->values, 0u);
  EXPECT_EQ(observation->errors, 1u);
  EXPECT_EQ(observation->stopped, 0u);
  EXPECT_FALSE(observation->saw_pause_code);
  EXPECT_EQ(observation->last_error, bnio::ssl::make_no_ssl_error());
  EXPECT_EQ(calls->load(std::memory_order_acquire), 0u);
}

TEST(SslHandshakeRetryTest, factory_terminal_error_completes_without_retry) {
  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);
  client_stream_type client{bnio::local::stream_socket(sockets[0]),
                            client_context};
  client_stream_type peer{bnio::local::stream_socket(sockets[1]),
                          client_context};

  auto record = std::make_shared<retry_record>();
  auto observation = std::make_shared<client_observation>();
  const std::error_code terminal =
      std::make_error_code(std::errc::permission_denied);

  // Exercise the 4-argument customization point object as the public
  // spelling of the retry surface.
  auto sender = bnio::ssl::async_handshake(
      scheduler, client, bnio::ssl::handshake_type::client,
      terminal_error_factory{record, terminal});
  auto operation = bexec::connect(
      std::move(sender), observing_handshake_receiver{observation, &context});
  bexec::start(operation);
  context.run();

  ASSERT_EQ(record->calls, 1u);
  ASSERT_EQ(record->pauses.size(), 1u);
  expect_pause_code(record->pauses.front());
  EXPECT_EQ(observation->values, 0u);
  EXPECT_EQ(observation->errors, 1u);
  EXPECT_EQ(observation->stopped, 0u);
  EXPECT_FALSE(observation->saw_pause_code);
  EXPECT_EQ(observation->last_error, terminal);
}

TEST(SslHandshakeRetryTest, pre_stopped_token_stops_without_factory) {
  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);
  client_stream_type client{bnio::local::stream_socket(sockets[0]),
                            client_context};
  client_stream_type peer{bnio::local::stream_socket(sockets[1]),
                          client_context};

  bexec::inplace_stop_source source;
  ASSERT_TRUE(source.request_stop());

  auto record = std::make_shared<retry_record>();
  auto observation = std::make_shared<client_observation>();
  stop_observing_handshake_receiver receiver;
  receiver.observation = observation;
  receiver.context = &context;
  receiver.env = stop_env{source.get_token()};

  auto sender = client.async_handshake(
      scheduler, bnio::ssl::handshake_type::client,
      recording_wait_factory{scheduler, &client, record, nullptr});
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(observation->values, 0u);
  EXPECT_EQ(observation->errors, 0u);
  EXPECT_EQ(observation->stopped, 1u);
  EXPECT_FALSE(observation->saw_pause_code);
  EXPECT_EQ(record->calls, 0u);
}

TEST(SslHandshakeRetryTest, stop_token_after_start_completes_stopped) {
  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);
  client_stream_type client{bnio::local::stream_socket(sockets[0]),
                            client_context};
  client_stream_type peer{bnio::local::stream_socket(sockets[1]),
                          client_context};

  bexec::inplace_stop_source source;
  auto record = std::make_shared<retry_record>();
  auto observation = std::make_shared<client_observation>();
  std::atomic<bool> paused{false};

  stop_observing_handshake_receiver receiver;
  receiver.observation = observation;
  // This test stops the context from the main thread. A completion delivered
  // during io_context::stop()'s drain must not re-enter stop(): the drain
  // thread would then spin waiting for the stop it is itself blocking.
  receiver.context = nullptr;
  receiver.env = stop_env{source.get_token()};

  auto sender = client.async_handshake(
      scheduler, bnio::ssl::handshake_type::client,
      recording_wait_factory{scheduler, &client, record, &paused});
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  std::thread worker([&context] { context.run(); });

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!paused.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() >= deadline) {
      (void)context.stop();
      worker.join();
      FAIL() << "the retry factory was never invoked";
    }
    std::this_thread::yield();
  }

  // Cancel immediately after the factory ran; no sleep-based handshake with
  // the worker. The stop token must win wherever the wait sender is: either
  // its transport read is already parked and is aborted, or it observes the
  // cancellation before starting transport work. Both deliver stopped.
  source.request_stop();
  (void)context.stop();
  worker.join();

  EXPECT_EQ(observation->values, 0u);
  EXPECT_EQ(observation->errors, 0u);
  EXPECT_EQ(observation->stopped, 1u);
  EXPECT_FALSE(observation->saw_pause_code);
  ASSERT_GE(record->calls, 1u);
  for (const auto& ec : record->pauses) {
    expect_pause_code(ec);
  }
}

TEST(SslHandshakeRetryTest,
     factory_runs_only_after_pause_output_reached_transport) {
  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);
  client_stream_type client{bnio::local::stream_socket(sockets[0]),
                            client_context};

  auto record = std::make_shared<retry_record>();
  auto observation = std::make_shared<client_observation>();
  auto drained = std::make_shared<std::atomic<bool>>(false);
  auto received = std::make_shared<std::atomic<bool>>(false);

  auto sender = client.async_handshake(
      scheduler, bnio::ssl::handshake_type::client,
      ordering_check_factory{scheduler, &client, sockets[1], record, drained,
                             received});
  auto operation = bexec::connect(
      std::move(sender), observing_handshake_receiver{observation, &context});
  bexec::start(operation);
  context.run();

  ASSERT_EQ(record->calls, 1u);
  expect_pause_code(record->pauses.front());
  EXPECT_TRUE(drained->load(std::memory_order_acquire))
      << "the factory saw pending write-BIO output; the pause flush was not "
         "completed first";
  EXPECT_TRUE(received->load(std::memory_order_acquire))
      << "the ClientHello had not reached the transport when the factory ran";
  EXPECT_EQ(observation->last_error,
            std::make_error_code(std::errc::permission_denied));
}

// Generates a real ClientHello with a standalone client SSL object. Its
// transport endpoint is never used: the returned hello is injected into the
// server's read BIO by the caller. The generator stream is destroyed here.
std::vector<char> generate_client_hello(bnio::ssl::context& client_context) {
  client_stream_type generator{bnio::local::stream_socket(-1), client_context};
  test_require(generator.valid());
  SSL* const generator_ssl = generator.native_handle().native_handle();
  test_require(generator_ssl != nullptr);
  SSL_set_connect_state(generator_ssl);
  ERR_clear_error();
  const int hello_result = SSL_do_handshake(generator_ssl);
  test_require(hello_result == -1);
  test_require(SSL_get_error(generator_ssl, hello_result) ==
               SSL_ERROR_WANT_READ);
  const std::size_t hello_size = BIO_ctrl_pending(generator.native_write_bio());
  test_require(hello_size > 0);
  std::vector<char> hello(hello_size);
  test_require(BIO_read(generator.native_write_bio(), hello.data(),
                        static_cast<int>(hello.size())) ==
               static_cast<int>(hello.size()));
  return hello;
}

// TLS 1.2 accepts a plaintext close_notify alert before the handshake keys are
// established. It sits in the transport and is read by the pause's want_read
// wait step; the next attempt then returns SSL_ERROR_ZERO_RETURN.
void send_plaintext_close_notify(int fd) {
  const unsigned char close_notify[] = {0x15, 0x03, 0x03, 0x00,
                                        0x02, 0x01, 0x00};
  test_require(::write(fd, close_notify, sizeof(close_notify)) ==
               static_cast<ssize_t>(sizeof(close_notify)));
}

TEST(SslHandshakeRetryTest,
     zero_return_after_pause_reports_connection_reset_without_flush) {
  test_certificate_files files;

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());
  test_require(!server_context.set_max_proto_version(TLS1_2_VERSION));

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);
  test_require(!client_context.set_max_proto_version(TLS1_2_VERSION));

  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  const std::vector<char> hello = generate_client_hello(client_context);

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);
  client_stream_type server{bnio::local::stream_socket(sockets[0]),
                            server_context};
  test_require(server.valid());

  // The first attempt consumes the ClientHello from the read BIO, so the
  // pause is want_read with the server flight queued for flushing.
  test_require(BIO_write(server.native_read_bio(), hello.data(),
                         static_cast<int>(hello.size())) ==
               static_cast<int>(hello.size()));

  send_plaintext_close_notify(sockets[1]);

  auto record = std::make_shared<retry_record>();
  auto observation = std::make_shared<client_observation>();

  auto sender = server.async_handshake(
      scheduler, bnio::ssl::handshake_type::server,
      recording_wait_factory{scheduler, &server, record, nullptr});
  auto operation = bexec::connect(
      std::move(sender), observing_handshake_receiver{observation, &context});
  bexec::start(operation);
  context.run();

  ASSERT_EQ(record->calls, 1u);
  expect_pause_code(record->pauses.front());
  EXPECT_EQ(observation->values, 0u);
  EXPECT_EQ(observation->errors, 1u);
  EXPECT_EQ(observation->stopped, 0u);
  EXPECT_FALSE(observation->saw_pause_code);
  // ZERO_RETURN is terminal with no flush, reported as connection_reset.
  EXPECT_EQ(observation->last_error,
            std::make_error_code(std::errc::connection_reset));
  EXPECT_EQ(BIO_ctrl_pending(server.native_write_bio()), std::size_t{0});
}

TEST(SslHandshakeRetryTest, lvalue_connected_client_completes_handshake) {
  test_certificate_files files;

  bnio::ssl::context server_context(bnio::ssl::context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());

  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);
  client_stream_type client{bnio::local::stream_socket(sockets[0]),
                            client_context};
  client_stream_type server{bnio::local::stream_socket(sockets[1]),
                            server_context};

  auto record = std::make_shared<retry_record>();
  auto state = std::make_shared<handshake_state>();
  handshake_receiver client_receiver{state, &context};
  handshake_receiver server_receiver{state, &context};

  auto client_sender = client.async_handshake(
      scheduler, bnio::ssl::handshake_type::client,
      recording_wait_factory{scheduler, &client, record, nullptr});
  auto server_sender =
      server.async_handshake(scheduler, bnio::ssl::handshake_type::server);

  // Connect the retry sender through an lvalue: the const& overload copies
  // the scheduler and factory into the operation. The sender lives until the
  // handshake finishes.
  auto client_operation =
      bexec::connect(client_sender, std::move(client_receiver));
  auto server_operation =
      bexec::connect(std::move(server_sender), std::move(server_receiver));

  bexec::start(client_operation);
  bexec::start(server_operation);
  context.run();

  EXPECT_EQ(state->values, 2u);
  EXPECT_EQ(state->errors, 0u);
  EXPECT_EQ(state->stopped, 0u);
  EXPECT_FALSE(state->error);
  ASSERT_GE(record->calls, 1u);
  for (const auto& ec : record->pauses) {
    expect_pause_code(ec);
  }
}

}  // namespace
