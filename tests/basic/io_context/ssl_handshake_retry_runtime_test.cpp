/**
 * @file ssl_handshake_retry_runtime_test.cpp
 * @brief Runtime unit tests for the factory-driven handshake retry step.
 *
 * These tests use the public API only. The retry operation drives transport
 * pauses internally, so the factory is only reached for callback-driven
 * pauses. A server-side stream with a preloaded ClientHello and a
 * client_hello callback returning retry pauses deterministically with
 * client_hello_cb, which lets the tests exercise the attempt-before-factory
 * order, the factory invocation contract, and the terminal mapping of factory
 * results without any live peer.
 */

#include <bnio/local.h>
#include <bnio/ssl.h>
#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <bexec/just.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <memory>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "../../support/io_context/ssl_test_support.h"

namespace {

using stream_type = bnio::ssl::tcp::stream<bnio::local::stream_socket>;
using scheduler_type = bnio::io_context::post_scheduler;
using bnio::ssl::handshake_pause_reason;
using bnio::ssl::handshake_pause_reason_of;
using bnio::ssl::handshake_type;
using bnio::ssl::is_handshake_pause;
using bnio::ssl::make_handshake_pause_error;

struct socketpair_holder {
  int sockets[2] = {-1, -1};

  socketpair_holder() {
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
      sockets[0] = -1;
      sockets[1] = -1;
    }
  }

  ~socketpair_holder() {
    for (int& fd : sockets) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }
  }

  socketpair_holder(const socketpair_holder&) = delete;
  socketpair_holder& operator=(const socketpair_holder&) = delete;

  [[nodiscard]] bool valid() const noexcept {
    return sockets[0] >= 0 && sockets[1] >= 0;
  }

  [[nodiscard]] int peer() const noexcept { return sockets[1]; }

  void close_peer() noexcept {
    if (sockets[1] >= 0) {
      ::close(sockets[1]);
      sockets[1] = -1;
    }
  }

  [[nodiscard]] int release_stream_end() noexcept {
    const int fd = sockets[0];
    sockets[0] = -1;
    return fd;
  }
};

struct retry_record {
  unsigned calls = 0;
  std::error_code last_pause;
};

struct public_result {
  unsigned values = 0;
  unsigned errors = 0;
  unsigned stopped = 0;
  std::error_code last_error;
  bool saw_pause_code = false;
};

struct result_receiver {
  public_result* result = nullptr;
  bnio::io_context* context = nullptr;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      ++result->errors;
      result->last_error = ec;
      if (is_handshake_pause(ec)) {
        result->saw_pause_code = true;
      }
    } else {
      ++result->values;
    }
    finish();
  }

  void set_stopped() noexcept {
    ++result->stopped;
    finish();
  }

 private:
  void finish() noexcept {
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

// Returns a chosen terminal error after recording the pause it received.
struct terminal_error_factory {
  retry_record* record = nullptr;
  std::error_code terminal;

  auto operator()(const std::error_code& ec) const {
    ++record->calls;
    record->last_pause = ec;
    return bexec::just(terminal);
  }
};

// Returns a pause code, which the retry operation must reject.
struct pause_code_factory {
  retry_record* record = nullptr;

  auto operator()(const std::error_code& ec) const {
    ++record->calls;
    record->last_pause = ec;
    return bexec::just(
        make_handshake_pause_error(handshake_pause_reason::want_read));
  }
};

// A factory whose invocation, sender move, and connect are all noexcept:
// this instantiates the retry_nothrow branch of the retry operation.
struct noexcept_terminal_factory {
  std::error_code terminal;

  auto operator()(const std::error_code&) const noexcept {
    return bexec::just(terminal);
  }
};

// A sender that advertises the retry contract but synchronously completes
// with set_stopped(). The concept requires exactly one value signature, so
// bexec::just_stopped() alone (zero value signatures) is not a valid factory
// result; this sender is.
struct sync_stopped_sender {
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                   bexec::set_stopped_t()>;

  template <class Receiver>
  struct operation {
    Receiver receiver;

    void start() noexcept { bexec::set_stopped(std::move(receiver)); }
  };

  template <class Receiver>
  operation<Receiver> connect(Receiver receiver) && {
    return operation<Receiver>{std::move(receiver)};
  }
};

// Completes with set_stopped() synchronously from the factory child.
struct stopped_factory {
  auto operator()(const std::error_code&) const {
    return sync_stopped_sender{};
  }
};

// Throws instead of returning a sender.
struct throwing_factory {
  retry_record* record = nullptr;

  bexec::just_sender<std::error_code> operator()(
      const std::error_code& ec) const {
    ++record->calls;
    record->last_pause = ec;
    throw std::runtime_error("retry factory failure");
  }
};

// A well-formed sender whose connect throws: the retry operation must treat
// the throw as a contract violation, not leak an exception or a pause code.
struct throwing_connect_operation {
  void start() noexcept {}
};

struct throwing_connect_sender {
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                   bexec::set_stopped_t()>;

  template <class Receiver>
  throwing_connect_operation connect(Receiver) && {
    throw std::runtime_error("retry connect failure");
  }
};

struct throwing_connect_factory {
  retry_record* record = nullptr;

  throwing_connect_sender operator()(const std::error_code& ec) const {
    ++record->calls;
    record->last_pause = ec;
    return {};
  }
};

// Generates a real ClientHello offering ALPN with a standalone client SSL
// object. The transport endpoint is never used: the hello is moved into the
// server's read BIO by the tests below.
[[nodiscard]] std::vector<char> make_client_hello_with_alpn() {
  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  EXPECT_TRUE(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  stream_type generator{bnio::local::stream_socket(-1), client_context};
  EXPECT_TRUE(generator.valid());
  SSL* const ssl = generator.native_handle().native_handle();
  EXPECT_NE(ssl, nullptr);

  static const unsigned char protocols[] = {2, 'h', '2'};
  EXPECT_EQ(SSL_set_alpn_protos(ssl, protocols, sizeof(protocols)), 0);
  SSL_set_connect_state(ssl);
  ERR_clear_error();
  const int result = SSL_do_handshake(ssl);
  EXPECT_EQ(result, -1);
  EXPECT_EQ(SSL_get_error(ssl, result), SSL_ERROR_WANT_READ);

  const std::size_t hello_size = BIO_ctrl_pending(generator.native_write_bio());
  EXPECT_GT(hello_size, std::size_t{0});
  std::vector<char> hello(hello_size);
  EXPECT_EQ(BIO_read(generator.native_write_bio(), hello.data(),
                     static_cast<int>(hello.size())),
            static_cast<int>(hello.size()));
  return hello;
}

// The server rejects every ALPN protocol with a fatal alert, which OpenSSL
// queues in the write BIO while the attempt fails.
void install_fatal_alpn_callback(bnio::ssl::context& context) {
  context.set_alpn_callback([](bnio::ssl::context_view, bnio::ssl::stream_view,
                               bnio::ssl::context::alpn_out&,
                               std::span<const unsigned char>) -> int {
    return SSL_TLSEXT_ERR_ALERT_FATAL;
  });
}

class SslHandshakeRetryRuntimeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!context_.is_open()) {
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    scheduler_ = context_.get_post_scheduler();
    ssl_context_ = std::make_unique<bnio::ssl::context>(
        bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(ssl_context_->valid());
  }

  // A fresh server-side stream. Tests that expect the factory to run preload
  // a ClientHello into its read BIO before starting.
  [[nodiscard]] std::unique_ptr<stream_type> make_server_stream() {
    if (!sockets_.valid()) {
      return nullptr;
    }
    return std::make_unique<stream_type>(
        bnio::local::stream_socket(sockets_.release_stream_end()),
        *ssl_context_);
  }

  // Installs a client_hello callback that always retries, so the first server
  // attempt pauses deterministically for a callback-driven reason.
  void install_retry_client_hello_callback() {
    ssl_context_->set_client_hello_callback(
        [](bnio::ssl::context_view, bnio::ssl::stream_view,
           bnio::ssl::alert_out&) -> bnio::ssl::client_hello_result {
          return bnio::ssl::client_hello_result::retry;
        });
  }

  // Preloads a complete ClientHello into the stream's read BIO; the server
  // attempt consumes it and reaches the client_hello callback.
  void preload_client_hello(stream_type& stream) {
    const std::vector<char> hello = make_client_hello_with_alpn();
    ASSERT_EQ(BIO_write(stream.native_read_bio(), hello.data(),
                        static_cast<int>(hello.size())),
              static_cast<int>(hello.size()));
  }

  bnio::io_context context_;
  scheduler_type scheduler_ = context_.get_post_scheduler();
  std::unique_ptr<bnio::ssl::context> ssl_context_;
  socketpair_holder sockets_;
};

TEST_F(SslHandshakeRetryRuntimeTest,
       attempt_pauses_before_factory_and_reports_terminal_value) {
  install_retry_client_hello_callback();
  auto stream = make_server_stream();
  ASSERT_NE(stream, nullptr);
  ASSERT_TRUE(stream->valid());
  preload_client_hello(*stream);

  retry_record record;
  public_result result;
  const std::error_code terminal =
      std::make_error_code(std::errc::permission_denied);

  auto sender =
      stream->async_handshake(scheduler_, handshake_type::server,
                              terminal_error_factory{&record, terminal});
  auto operation =
      bexec::connect(std::move(sender), result_receiver{&result, &context_});
  bexec::start(operation);
  context_.run();

  // The attempt ran first and paused in the client_hello callback; the
  // factory saw exactly that pause.
  EXPECT_EQ(record.calls, 1U);
  ASSERT_TRUE(is_handshake_pause(record.last_pause));
  EXPECT_EQ(handshake_pause_reason_of(record.last_pause),
            handshake_pause_reason::client_hello_cb);

  // The factory's non-empty result is the terminal value, and the pause code
  // never reaches the public receiver.
  EXPECT_EQ(result.values, 0U);
  EXPECT_EQ(result.errors, 1U);
  EXPECT_EQ(result.stopped, 0U);
  EXPECT_FALSE(result.saw_pause_code);
  EXPECT_EQ(result.last_error, terminal);
}

TEST_F(SslHandshakeRetryRuntimeTest,
       factory_returned_pause_code_is_protocol_error) {
  install_retry_client_hello_callback();
  auto stream = make_server_stream();
  ASSERT_NE(stream, nullptr);
  ASSERT_TRUE(stream->valid());
  preload_client_hello(*stream);

  retry_record record;
  public_result result;

  auto sender = stream->async_handshake(scheduler_, handshake_type::server,
                                        pause_code_factory{&record});
  auto operation =
      bexec::connect(std::move(sender), result_receiver{&result, &context_});
  bexec::start(operation);
  context_.run();

  EXPECT_EQ(record.calls, 1U);
  EXPECT_TRUE(is_handshake_pause(record.last_pause));
  EXPECT_EQ(handshake_pause_reason_of(record.last_pause),
            handshake_pause_reason::client_hello_cb);
  EXPECT_EQ(result.values, 0U);
  EXPECT_EQ(result.errors, 1U);
  EXPECT_EQ(result.stopped, 0U);
  EXPECT_FALSE(result.saw_pause_code);
  EXPECT_EQ(result.last_error, std::make_error_code(std::errc::protocol_error));
}

TEST_F(SslHandshakeRetryRuntimeTest,
       throwing_factory_maps_to_protocol_error_without_leaking) {
  install_retry_client_hello_callback();
  auto stream = make_server_stream();
  ASSERT_NE(stream, nullptr);
  ASSERT_TRUE(stream->valid());
  preload_client_hello(*stream);

  retry_record record;
  public_result result;

  auto sender = stream->async_handshake(scheduler_, handshake_type::server,
                                        throwing_factory{&record});
  auto operation =
      bexec::connect(std::move(sender), result_receiver{&result, &context_});
  bexec::start(operation);
  context_.run();

  EXPECT_EQ(record.calls, 1U);
  EXPECT_TRUE(is_handshake_pause(record.last_pause));
  EXPECT_EQ(handshake_pause_reason_of(record.last_pause),
            handshake_pause_reason::client_hello_cb);
  EXPECT_EQ(result.values, 0U);
  EXPECT_EQ(result.errors, 1U);
  EXPECT_EQ(result.stopped, 0U);
  EXPECT_FALSE(result.saw_pause_code);
  EXPECT_EQ(result.last_error, std::make_error_code(std::errc::protocol_error));
}

TEST_F(SslHandshakeRetryRuntimeTest,
       throwing_connect_maps_to_protocol_error_without_leaking) {
  install_retry_client_hello_callback();
  auto stream = make_server_stream();
  ASSERT_NE(stream, nullptr);
  ASSERT_TRUE(stream->valid());
  preload_client_hello(*stream);

  retry_record record;
  public_result result;

  auto sender = stream->async_handshake(scheduler_, handshake_type::server,
                                        throwing_connect_factory{&record});
  auto operation =
      bexec::connect(std::move(sender), result_receiver{&result, &context_});
  bexec::start(operation);
  context_.run();

  EXPECT_EQ(record.calls, 1U);
  EXPECT_TRUE(is_handshake_pause(record.last_pause));
  EXPECT_EQ(handshake_pause_reason_of(record.last_pause),
            handshake_pause_reason::client_hello_cb);
  EXPECT_EQ(result.values, 0U);
  EXPECT_EQ(result.errors, 1U);
  EXPECT_EQ(result.stopped, 0U);
  EXPECT_FALSE(result.saw_pause_code);
  EXPECT_EQ(result.last_error, std::make_error_code(std::errc::protocol_error));
}

TEST_F(SslHandshakeRetryRuntimeTest,
       fatal_attempt_flushes_the_alert_and_never_calls_the_factory) {
  auto stream = make_server_stream();
  ASSERT_NE(stream, nullptr);
  ASSERT_TRUE(stream->valid());

  // A real ClientHello whose ALPN offer the server rejects with ALERT_FATAL:
  // the first attempt fails fatally with a fatal alert queued in the write
  // BIO, before any factory could be involved.
  install_fatal_alpn_callback(*ssl_context_);
  const std::vector<char> hello = make_client_hello_with_alpn();
  ASSERT_GT(BIO_write(stream->native_read_bio(), hello.data(),
                      static_cast<int>(hello.size())),
            0);

  retry_record record;
  public_result result;

  auto sender = stream->async_handshake(
      scheduler_, handshake_type::server,
      terminal_error_factory{&record,
                             std::make_error_code(std::errc::io_error)});
  auto operation =
      bexec::connect(std::move(sender), result_receiver{&result, &context_});
  bexec::start(operation);
  context_.run();

  // The fatal alert queued by OpenSSL was flushed to the transport before the
  // error was reported; the factory was never invoked.
  EXPECT_EQ(record.calls, 0U);
  EXPECT_EQ(result.values, 0U);
  EXPECT_EQ(result.errors, 1U);
  EXPECT_EQ(result.stopped, 0U);
  EXPECT_FALSE(result.saw_pause_code);
  EXPECT_TRUE(result.last_error);
  EXPECT_EQ(result.last_error.category(), bnio::ssl::openssl_error_category());

  // The flush actually happened: the alert left the write BIO and reached
  // the peer. Without these the test would still pass if the flush were
  // deleted entirely.
  EXPECT_EQ(BIO_ctrl_pending(stream->native_write_bio()), std::size_t{0});
  std::array<char, 64> alert{};
  const ssize_t received =
      ::recv(sockets_.peer(), alert.data(), alert.size(), MSG_DONTWAIT);
  ASSERT_GT(received, 0) << "the fatal alert never reached the peer";
  EXPECT_EQ(static_cast<unsigned char>(alert[0]), 0x15U)
      << "the first flushed record is not a TLS alert";
}

TEST_F(SslHandshakeRetryRuntimeTest,
       transport_error_wins_over_staged_ssl_error) {
  auto stream = make_server_stream();
  ASSERT_NE(stream, nullptr);
  ASSERT_TRUE(stream->valid());

  // Force a fatal SSL error with a queued alert, then take the transport
  // away: the alert flush fails, and the transport error must be reported
  // instead of the staged OpenSSL error.
  install_fatal_alpn_callback(*ssl_context_);
  const std::vector<char> hello = make_client_hello_with_alpn();
  ASSERT_GT(BIO_write(stream->native_read_bio(), hello.data(),
                      static_cast<int>(hello.size())),
            0);
  sockets_.close_peer();

  retry_record record;
  public_result result;

  auto sender = stream->async_handshake(
      scheduler_, handshake_type::server,
      terminal_error_factory{&record,
                             std::make_error_code(std::errc::io_error)});
  auto operation =
      bexec::connect(std::move(sender), result_receiver{&result, &context_});
  bexec::start(operation);
  context_.run();

  EXPECT_EQ(record.calls, 0U);
  EXPECT_EQ(result.values, 0U);
  EXPECT_EQ(result.errors, 1U);
  EXPECT_EQ(result.stopped, 0U);
  EXPECT_FALSE(result.saw_pause_code);
  ASSERT_TRUE(result.last_error);
  EXPECT_NE(result.last_error.category(), bnio::ssl::openssl_error_category())
      << "the transport failure must win over the staged SSL error";
}

TEST_F(SslHandshakeRetryRuntimeTest,
       invalid_stream_survives_inline_post_after_context_stop) {
  auto owner = make_server_stream();
  ASSERT_NE(owner, nullptr);
  ASSERT_TRUE(owner->valid());

  // Invalidate a stream but keep a valid owner, then stop the context before
  // starting. The post scheduler rejects the terminal post and completes it
  // inline; the staged no-OpenSSL-error completion must still be delivered
  // unchanged, and the deferred driver post must not touch freed state.
  stream_type moved_out{std::move(*owner)};
  ASSERT_FALSE(owner->valid());
  ASSERT_TRUE(moved_out.valid());
  (void)context_.stop();

  retry_record record;
  public_result result;

  auto sender = owner->async_handshake(
      scheduler_, handshake_type::client,
      terminal_error_factory{&record,
                             std::make_error_code(std::errc::io_error)});
  // The context is already stopped: do not re-enter stop() from the receiver.
  auto operation =
      bexec::connect(std::move(sender), result_receiver{&result, nullptr});
  bexec::start(operation);
  context_.run();

  EXPECT_EQ(record.calls, 0U);
  EXPECT_EQ(result.values, 0U);
  EXPECT_EQ(result.errors, 1U);
  EXPECT_EQ(result.stopped, 0U);
  EXPECT_FALSE(result.saw_pause_code);
  EXPECT_EQ(result.last_error, bnio::ssl::make_no_ssl_error());
}

TEST_F(SslHandshakeRetryRuntimeTest,
       synchronous_factory_stopped_stops_without_retry) {
  install_retry_client_hello_callback();
  auto stream = make_server_stream();
  ASSERT_NE(stream, nullptr);
  ASSERT_TRUE(stream->valid());
  preload_client_hello(*stream);

  public_result result;
  auto sender = stream->async_handshake(scheduler_, handshake_type::server,
                                        stopped_factory{});
  auto operation =
      bexec::connect(std::move(sender), result_receiver{&result, &context_});
  bexec::start(operation);
  context_.run();

  EXPECT_EQ(result.values, 0U);
  EXPECT_EQ(result.errors, 0U);
  EXPECT_EQ(result.stopped, 1U);
  EXPECT_FALSE(result.saw_pause_code);
}

TEST_F(SslHandshakeRetryRuntimeTest,
       noexcept_factory_runs_the_nothrow_builder_path) {
  install_retry_client_hello_callback();
  auto stream = make_server_stream();
  ASSERT_NE(stream, nullptr);
  ASSERT_TRUE(stream->valid());
  preload_client_hello(*stream);

  retry_record record;
  public_result result;
  const std::error_code terminal =
      std::make_error_code(std::errc::permission_denied);

  auto sender = stream->async_handshake(scheduler_, handshake_type::server,
                                        noexcept_terminal_factory{terminal});
  auto operation =
      bexec::connect(std::move(sender), result_receiver{&result, &context_});
  bexec::start(operation);
  context_.run();

  EXPECT_EQ(result.values, 0U);
  EXPECT_EQ(result.errors, 1U);
  EXPECT_EQ(result.stopped, 0U);
  EXPECT_FALSE(result.saw_pause_code);
  EXPECT_EQ(result.last_error, terminal);
}

TEST_F(SslHandshakeRetryRuntimeTest, lvalue_connect_runs_the_sender_copy_path) {
  install_retry_client_hello_callback();
  auto stream = make_server_stream();
  ASSERT_NE(stream, nullptr);
  ASSERT_TRUE(stream->valid());
  preload_client_hello(*stream);

  retry_record record;
  public_result result;
  const std::error_code terminal =
      std::make_error_code(std::errc::permission_denied);

  auto sender =
      stream->async_handshake(scheduler_, handshake_type::server,
                              terminal_error_factory{&record, terminal});
  // Connect through a non-const lvalue: the sender copies its scheduler and
  // factory into the operation. The sender must outlive the operation.
  auto operation = bexec::connect(sender, result_receiver{&result, &context_});
  bexec::start(operation);
  context_.run();

  EXPECT_EQ(record.calls, 1U);
  EXPECT_EQ(handshake_pause_reason_of(record.last_pause),
            handshake_pause_reason::client_hello_cb);
  EXPECT_EQ(result.values, 0U);
  EXPECT_EQ(result.errors, 1U);
  EXPECT_EQ(result.stopped, 0U);
  EXPECT_EQ(result.last_error, terminal);
}

TEST_F(SslHandshakeRetryRuntimeTest,
       callback_pause_terminates_the_2arg_overload_as_unsupported) {
  install_retry_client_hello_callback();
  auto stream = make_server_stream();
  ASSERT_NE(stream, nullptr);
  ASSERT_TRUE(stream->valid());
  preload_client_hello(*stream);

  public_result result;
  auto sender = stream->async_handshake(scheduler_, handshake_type::server);
  auto operation =
      bexec::connect(std::move(sender), result_receiver{&result, &context_});
  bexec::start(operation);
  context_.run();

  // The 2-argument overload is a retry sender with the stateless default
  // factory: transport pauses are driven internally, and a callback-driven
  // pause terminates with operation_not_supported because this overload has
  // no resume surface. The pause code never reaches the public receiver.
  EXPECT_EQ(result.values, 0U);
  EXPECT_EQ(result.errors, 1U);
  EXPECT_EQ(result.stopped, 0U);
  EXPECT_FALSE(result.saw_pause_code);
  EXPECT_EQ(result.last_error,
            std::make_error_code(std::errc::operation_not_supported));
}

}  // namespace
