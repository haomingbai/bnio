/**
 * @file ssl_handshake_wait_test.cpp
 * @brief Basic unit tests for the SSL handshake BIO wait helper.
 *
 * The wait helper's BIO plumbing is internal, so these tests include the
 * public umbrella plus OpenSSL and observe the stream's BIOs directly. Real
 * transport steps are driven by an io_context over a socketpair; no sleep is
 * used for synchronization.
 */

#include <bnio/local.h>
#include <bnio/ssl.h>
#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <bexec/completion_signatures.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/receiver.hpp>
#include <bexec/sender.hpp>
#include <bexec/stop_token.hpp>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "../../support/io_context/ssl_test_support.h"

namespace {

using bnio::ssl::handshake_pause_reason;
using bnio::ssl::handshake_pause_reason_of;
using bnio::ssl::is_handshake_pause;
using bnio::ssl::make_handshake_pause_error;
using stream_type = bnio::ssl::tcp::stream<bnio::local::stream_socket>;
using scheduler_type = bnio::io_context::post_scheduler;

struct wait_observation {
  unsigned terminals = 0;
  unsigned values = 0;
  unsigned stopped = 0;
  std::error_code error;
};

// Public receiver for the wait sender: exactly set_value(std::error_code) and
// set_stopped(), with an optional context stop so io_context::run() returns.
struct wait_receiver {
  wait_observation* observation = nullptr;
  bnio::io_context* context = nullptr;

  void set_value(std::error_code ec) noexcept {
    ++observation->values;
    ++observation->terminals;
    observation->error = ec;
    finish();
  }

  void set_stopped() noexcept {
    ++observation->stopped;
    ++observation->terminals;
    finish();
  }

 private:
  void finish() noexcept {
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct stopping_wait_receiver : wait_receiver {
  stop_env env;

  [[nodiscard]] stop_env get_env() const noexcept { return env; }
};

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

  [[nodiscard]] int release_stream_end() noexcept {
    const int fd = sockets[0];
    sockets[0] = -1;
    return fd;
  }

  void close_peer() noexcept {
    if (sockets[1] >= 0) {
      ::close(sockets[1]);
      sockets[1] = -1;
    }
  }
};

using wait_sender_type = decltype(bnio::ssl::make_handshake_wait_sender(
    std::declval<scheduler_type>(), std::declval<stream_type&>(),
    std::declval<std::error_code>()));
using wait_completion_signatures =
    bexec::completion_signatures_of_t<wait_sender_type>;

// Contract: exactly one value signature carrying the error_code, one stopped
// signature, and no error channel at all.
static_assert(bexec::sender<wait_sender_type>);
static_assert(
    wait_completion_signatures::template count_of<bexec::set_value_t>() == 1);
static_assert(
    wait_completion_signatures::template count_of<bexec::set_stopped_t>() == 1);
static_assert(
    wait_completion_signatures::template count_of<bexec::set_error_t>() == 0);
static_assert(bexec::receiver_of<wait_receiver, wait_completion_signatures>);
static_assert(
    bexec::operation_state<decltype(bexec::connect(
        std::declval<wait_sender_type>(), std::declval<wait_receiver>()))>);
// The sender is copyable (scheduler is copyable), so lvalue connect works too.
static_assert(
    bexec::operation_state<decltype(bexec::connect(
        std::declval<wait_sender_type&>(), std::declval<wait_receiver>()))>);

// The factory must also instantiate for the default TCP-socket stream, not
// just the local socketpair stream used by these tests.
using tcp_stream_type = bnio::ssl::tcp::stream<bnio::tcp_socket>;
using tcp_wait_sender_type = decltype(bnio::ssl::make_handshake_wait_sender(
    std::declval<scheduler_type>(), std::declval<tcp_stream_type&>(),
    std::declval<std::error_code>()));
static_assert(bexec::sender<tcp_wait_sender_type>);
static_assert(
    bexec::operation_state<decltype(bexec::connect(
        std::declval<tcp_wait_sender_type>(), std::declval<wait_receiver>()))>);

TEST(SslHandshakeWaitTest, non_pause_error_is_invalid_argument) {
  bnio::io_context context;
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());

  socketpair_holder sockets;
  ASSERT_TRUE(sockets.valid());
  stream_type stream{bnio::local::stream_socket(sockets.release_stream_end()),
                     ssl_context};
  ASSERT_TRUE(stream.valid());

  const std::error_code non_pause_codes[] = {
      std::error_code{},
      std::make_error_code(std::errc::permission_denied),
      bnio::ssl::make_no_ssl_error(),
      // Value zero in the pause category carries no pause.
      std::error_code{0, bnio::ssl::handshake_pause_category()},
  };
  for (const std::error_code& code : non_pause_codes) {
    ASSERT_FALSE(is_handshake_pause(code));
    wait_observation observation;
    auto sender =
        bnio::ssl::make_handshake_wait_sender(scheduler, stream, code);
    auto operation =
        bexec::connect(std::move(sender), wait_receiver{&observation});
    bexec::start(operation);

    // Classification happens before any BIO or transport work, so the
    // completion is synchronous and no io_context::run() is needed.
    EXPECT_EQ(observation.terminals, 1U);
    EXPECT_EQ(observation.values, 1U);
    EXPECT_EQ(observation.stopped, 0U);
    EXPECT_EQ(observation.error,
              std::make_error_code(std::errc::invalid_argument));
  }
}

TEST(SslHandshakeWaitTest, invalid_stream_reports_no_ssl_error) {
  bnio::io_context context;
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());

  socketpair_holder sockets;
  ASSERT_TRUE(sockets.valid());
  stream_type source{bnio::local::stream_socket(sockets.release_stream_end()),
                     ssl_context};
  ASSERT_TRUE(source.valid());
  stream_type owner{std::move(source)};
  ASSERT_FALSE(source.valid());

  wait_observation observation;
  auto sender = bnio::ssl::make_handshake_wait_sender(
      scheduler, source,
      make_handshake_pause_error(handshake_pause_reason::want_read));
  auto operation =
      bexec::connect(std::move(sender), wait_receiver{&observation});
  bexec::start(operation);

  EXPECT_EQ(observation.terminals, 1U);
  EXPECT_EQ(observation.values, 1U);
  EXPECT_EQ(observation.stopped, 0U);
  // No BIO exists on a moved-from stream, so no OpenSSL call is attempted.
  EXPECT_EQ(observation.error, bnio::ssl::make_no_ssl_error());
  EXPECT_FALSE(is_handshake_pause(observation.error));
}

TEST(SslHandshakeWaitTest,
     invalid_stream_takes_priority_over_non_pause_classification) {
  bnio::io_context context;
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());

  socketpair_holder sockets;
  ASSERT_TRUE(sockets.valid());
  stream_type source{bnio::local::stream_socket(sockets.release_stream_end()),
                     ssl_context};
  ASSERT_TRUE(source.valid());
  stream_type owner{std::move(source)};
  ASSERT_FALSE(source.valid());

  // The stream invalidity check runs before pause classification: a non-pause
  // code must not turn this into invalid_argument.
  wait_observation observation;
  auto sender = bnio::ssl::make_handshake_wait_sender(
      scheduler, source, std::make_error_code(std::errc::permission_denied));
  auto operation =
      bexec::connect(std::move(sender), wait_receiver{&observation});
  bexec::start(operation);

  EXPECT_EQ(observation.terminals, 1U);
  EXPECT_EQ(observation.values, 1U);
  EXPECT_EQ(observation.stopped, 0U);
  EXPECT_EQ(observation.error, bnio::ssl::make_no_ssl_error());
  EXPECT_FALSE(is_handshake_pause(observation.error));
}

TEST(SslHandshakeWaitTest, callback_pauses_are_operation_not_supported) {
  bnio::io_context context;
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());

  socketpair_holder sockets;
  ASSERT_TRUE(sockets.valid());
  stream_type stream{bnio::local::stream_socket(sockets.release_stream_end()),
                     ssl_context};
  ASSERT_TRUE(stream.valid());

  const handshake_pause_reason callback_reasons[] = {
      handshake_pause_reason::x509_lookup,
      handshake_pause_reason::async,
      handshake_pause_reason::async_job,
      handshake_pause_reason::client_hello_cb,
      handshake_pause_reason::retry_verify,
  };
  for (const handshake_pause_reason reason : callback_reasons) {
    const std::error_code pause = make_handshake_pause_error(reason);
    ASSERT_TRUE(is_handshake_pause(pause));
    wait_observation observation;
    auto sender =
        bnio::ssl::make_handshake_wait_sender(scheduler, stream, pause);
    auto operation =
        bexec::connect(std::move(sender), wait_receiver{&observation});
    bexec::start(operation);

    EXPECT_EQ(observation.terminals, 1U);
    EXPECT_EQ(observation.values, 1U);
    EXPECT_EQ(observation.stopped, 0U);
    EXPECT_EQ(observation.error,
              std::make_error_code(std::errc::operation_not_supported));
    EXPECT_FALSE(is_handshake_pause(observation.error));
  }

  // A nonzero value in the pause category that names no known reason has no
  // BIO work this helper can do either.
  const std::error_code unknown_pause{42,
                                      bnio::ssl::handshake_pause_category()};
  ASSERT_TRUE(is_handshake_pause(unknown_pause));
  ASSERT_EQ(handshake_pause_reason_of(unknown_pause),
            handshake_pause_reason::none);
  wait_observation observation;
  auto sender =
      bnio::ssl::make_handshake_wait_sender(scheduler, stream, unknown_pause);
  auto operation =
      bexec::connect(std::move(sender), wait_receiver{&observation});
  bexec::start(operation);
  EXPECT_EQ(observation.values, 1U);
  EXPECT_EQ(observation.error,
            std::make_error_code(std::errc::operation_not_supported));
}

TEST(SslHandshakeWaitTest, want_read_commits_transport_bytes_into_read_bio) {
  bnio::io_context context;
  if (!context.is_open()) {
    return;
  }
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());

  socketpair_holder sockets;
  ASSERT_TRUE(sockets.valid());
  stream_type stream{bnio::local::stream_socket(sockets.release_stream_end()),
                     ssl_context};
  ASSERT_TRUE(stream.valid());

  wait_observation observation;
  auto sender = bnio::ssl::make_handshake_wait_sender(
      scheduler, stream,
      make_handshake_pause_error(handshake_pause_reason::want_read));
  auto operation =
      bexec::connect(std::move(sender), wait_receiver{&observation, &context});
  bexec::start(operation);

  // The only synchronization used is the io_context itself: the read starts
  // pending, the peer writes, and run() delivers the completion.
  constexpr std::string_view payload = "handshake-wait-bytes";
  const ssize_t written =
      ::write(sockets.peer(), payload.data(), payload.size());
  ASSERT_EQ(written, static_cast<ssize_t>(payload.size()));

  context.run();

  EXPECT_EQ(observation.terminals, 1U);
  EXPECT_EQ(observation.values, 1U);
  EXPECT_EQ(observation.stopped, 0U);
  EXPECT_FALSE(observation.error);
  EXPECT_FALSE(is_handshake_pause(observation.error));

  SSL* const ssl = stream.native_handle().native_handle();
  ASSERT_NE(ssl, nullptr);
  BIO* const read_bio = SSL_get_rbio(ssl);
  ASSERT_NE(read_bio, nullptr);
  std::array<char, 64> received{};
  const int read =
      BIO_read(read_bio, received.data(), static_cast<int>(received.size()));
  ASSERT_EQ(read, static_cast<int>(payload.size()));
  EXPECT_EQ(std::string_view(received.data(), static_cast<std::size_t>(read)),
            payload);
}

TEST(SslHandshakeWaitTest, want_read_zero_bytes_reports_connection_reset) {
  bnio::io_context context;
  if (!context.is_open()) {
    return;
  }
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());

  socketpair_holder sockets;
  ASSERT_TRUE(sockets.valid());
  stream_type stream{bnio::local::stream_socket(sockets.release_stream_end()),
                     ssl_context};
  ASSERT_TRUE(stream.valid());
  // EOF on the transport read: a clean peer close yields zero bytes with an
  // empty error code, which must surface as connection_reset rather than a
  // successful wait.
  sockets.close_peer();

  wait_observation observation;
  auto sender = bnio::ssl::make_handshake_wait_sender(
      scheduler, stream,
      make_handshake_pause_error(handshake_pause_reason::want_read));
  auto operation =
      bexec::connect(std::move(sender), wait_receiver{&observation, &context});
  bexec::start(operation);
  context.run();

  EXPECT_EQ(observation.terminals, 1U);
  EXPECT_EQ(observation.values, 1U);
  EXPECT_EQ(observation.stopped, 0U);
  EXPECT_EQ(observation.error,
            std::make_error_code(std::errc::connection_reset));
}

TEST(SslHandshakeWaitTest, want_read_forwards_downstream_stop_token) {
  bnio::io_context context;
  if (!context.is_open()) {
    return;
  }
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());

  socketpair_holder sockets;
  ASSERT_TRUE(sockets.valid());
  stream_type stream{bnio::local::stream_socket(sockets.release_stream_end()),
                     ssl_context};
  ASSERT_TRUE(stream.valid());

  bexec::inplace_stop_source stop_source;
  ASSERT_TRUE(stop_source.request_stop());

  wait_observation observation;
  stopping_wait_receiver receiver;
  receiver.observation = &observation;
  receiver.context = &context;
  receiver.env.token = stop_source.get_token();

  auto sender = bnio::ssl::make_handshake_wait_sender(
      scheduler, stream,
      make_handshake_pause_error(handshake_pause_reason::want_read));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  // The environment reaches the transport operation, which reports stopped;
  // the wait helper forwards it instead of fabricating a value completion.
  EXPECT_EQ(observation.terminals, 1U);
  EXPECT_EQ(observation.values, 0U);
  EXPECT_EQ(observation.stopped, 1U);
}

TEST(SslHandshakeWaitTest, want_write_with_empty_bio_completes_synchronously) {
  bnio::io_context context;
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());

  socketpair_holder sockets;
  ASSERT_TRUE(sockets.valid());
  stream_type stream{bnio::local::stream_socket(sockets.release_stream_end()),
                     ssl_context};
  ASSERT_TRUE(stream.valid());
  ASSERT_NE(stream.native_write_bio(), nullptr);
  ASSERT_EQ(BIO_ctrl_pending(stream.native_write_bio()), std::size_t{0});

  wait_observation observation;
  // Connect through an lvalue sender on purpose: the copyable scheduler must
  // support const& connect as well as prvalue connect.
  auto sender = bnio::ssl::make_handshake_wait_sender(
      scheduler, stream,
      make_handshake_pause_error(handshake_pause_reason::want_write));
  auto operation = bexec::connect(sender, wait_receiver{&observation});
  bexec::start(operation);

  // There is no pending output, so no transport operation is started and the
  // completion is delivered synchronously.
  EXPECT_EQ(observation.terminals, 1U);
  EXPECT_EQ(observation.values, 1U);
  EXPECT_EQ(observation.stopped, 0U);
  EXPECT_FALSE(observation.error);
}

TEST(SslHandshakeWaitTest,
     want_write_shut_down_write_half_reports_empty_not_an_error) {
  bnio::io_context context;
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());

  socketpair_holder sockets;
  ASSERT_TRUE(sockets.valid());
  stream_type stream{bnio::local::stream_socket(sockets.release_stream_end()),
                     ssl_context};
  ASSERT_TRUE(stream.valid());
  ASSERT_NE(stream.native_write_bio(), nullptr);

  // Close the SSL side of the write pair. BIO_nread0 then reports 0 (EOF),
  // which the wait helper must treat like the -1 "no data" state: nothing
  // pending, so the step completes successfully with an empty error.
  SSL* const ssl = stream.native_handle().native_handle();
  ASSERT_NE(ssl, nullptr);
  ASSERT_EQ(BIO_shutdown_wr(SSL_get_wbio(ssl)), 1);
  char* data = nullptr;
  ASSERT_LE(BIO_nread0(stream.native_write_bio(), &data), 0);

  wait_observation observation;
  auto sender = bnio::ssl::make_handshake_wait_sender(
      scheduler, stream,
      make_handshake_pause_error(handshake_pause_reason::want_write));
  auto operation =
      bexec::connect(std::move(sender), wait_receiver{&observation});
  bexec::start(operation);

  EXPECT_EQ(observation.terminals, 1U);
  EXPECT_EQ(observation.values, 1U);
  EXPECT_EQ(observation.stopped, 0U);
  EXPECT_FALSE(observation.error);
}

TEST(SslHandshakeWaitTest, want_write_drains_queued_bio_output) {
  bnio::io_context context;
  if (!context.is_open()) {
    return;
  }
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());
  ssl_context.set_verify_mode(SSL_VERIFY_NONE);

  socketpair_holder sockets;
  ASSERT_TRUE(sockets.valid());
  stream_type stream{bnio::local::stream_socket(sockets.release_stream_end()),
                     ssl_context};
  ASSERT_TRUE(stream.valid());

  // Drive a client handshake far enough to queue a ClientHello in the write
  // BIO without a server present; that is the exact state a want_write pause
  // must flush.
  SSL* const ssl = stream.native_handle().native_handle();
  ASSERT_NE(ssl, nullptr);
  SSL_set_connect_state(ssl);
  ERR_clear_error();
  const int handshake_result = SSL_do_handshake(ssl);
  ASSERT_EQ(handshake_result, -1);
  EXPECT_EQ(SSL_get_error(ssl, handshake_result), SSL_ERROR_WANT_READ);
  const std::size_t pending = BIO_ctrl_pending(stream.native_write_bio());
  ASSERT_GT(pending, std::size_t{0});

  wait_observation observation;
  auto sender = bnio::ssl::make_handshake_wait_sender(
      scheduler, stream,
      make_handshake_pause_error(handshake_pause_reason::want_write));
  auto operation =
      bexec::connect(std::move(sender), wait_receiver{&observation, &context});
  bexec::start(operation);
  context.run();

  EXPECT_EQ(observation.terminals, 1U);
  EXPECT_EQ(observation.values, 1U);
  EXPECT_EQ(observation.stopped, 0U);
  EXPECT_FALSE(observation.error);
  EXPECT_EQ(BIO_ctrl_pending(stream.native_write_bio()), std::size_t{0});

  // Everything the helper drained from the write BIO reached the peer; run()
  // returned only after the completion, so the bytes are already queued.
  std::array<char, 65536> peer_bytes{};
  const ssize_t received = ::recv(sockets.peer(), peer_bytes.data(),
                                  peer_bytes.size(), MSG_DONTWAIT);
  ASSERT_GE(received, 0);
  EXPECT_EQ(static_cast<std::size_t>(received), pending);
}

// ---------------------------------------------------------------------------
// Synchronous transport: proves the wait operation tolerates a transport that
// completes inside start(). bnio's io_uring transports always complete
// asynchronously, but the helper is templated on the next layer and scheduler,
// so the inline-completion half of the epoch handoff must work as well.
// ---------------------------------------------------------------------------

template <class Receiver>
struct fake_read_operation {
  Receiver receiver;
  bnio::mutable_buffer buffer;
  const char* data = nullptr;
  std::size_t size = 0;
  std::size_t reported_extra = 0;

  void start() noexcept {
    const std::size_t copied = size < buffer.size() ? size : buffer.size();
    if (copied != 0) {
      std::memcpy(buffer.data(), data, copied);
    }
    bexec::set_value(std::move(receiver), std::error_code{},
                     copied + reported_extra);
  }
};

struct fake_read_sender {
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  const char* data = nullptr;
  std::size_t size = 0;
  bnio::mutable_buffer buffer;
  std::size_t reported_extra = 0;

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return fake_read_operation<Receiver>{std::move(receiver), buffer, data,
                                         size, reported_extra};
  }
};

template <class Receiver>
struct fake_write_operation {
  Receiver receiver;
  bnio::const_buffer buffer;
  std::string* captured = nullptr;
  std::size_t chunk = 0;
  std::size_t reported_extra = 0;

  void start() noexcept {
    std::size_t accepted = buffer.size();
    if (chunk != 0 && chunk < accepted) {
      accepted = chunk;
    }
    if (accepted != 0) {
      captured->append(static_cast<const char*>(buffer.data()), accepted);
    }
    bexec::set_value(std::move(receiver), std::error_code{},
                     accepted + reported_extra);
  }
};

struct fake_write_sender {
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  bnio::const_buffer buffer;
  std::string* captured = nullptr;
  std::size_t chunk = 0;
  std::size_t reported_extra = 0;

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return fake_write_operation<Receiver>{std::move(receiver), buffer, captured,
                                          chunk, reported_extra};
  }
};

// Minimal next layer: no scheduler interaction, transport steps complete
// synchronously from start(). reported_extra lets a test break the transport
// contract on purpose to reach the BIO invariant checks.
struct fake_synchronous_transport {
  std::string_view inbound;
  std::string captured;
  std::size_t write_chunk = 0;
  std::size_t read_reported_extra = 0;
  std::size_t write_reported_extra = 0;

  fake_synchronous_transport& lowest_layer() noexcept { return *this; }
  const fake_synchronous_transport& lowest_layer() const noexcept {
    return *this;
  }

  template <class Scheduler>
  auto async_read_some(Scheduler&, bnio::mutable_buffer buffer) {
    return fake_read_sender{inbound.data(), inbound.size(), buffer,
                            read_reported_extra};
  }

  template <class Scheduler>
  auto async_write_some(Scheduler&, bnio::const_buffer buffer, int) {
    return fake_write_sender{buffer, &captured, write_chunk,
                             write_reported_extra};
  }
};

using fake_stream_type = bnio::ssl::tcp::stream<fake_synchronous_transport>;

TEST(SslHandshakeWaitTest,
     synchronous_read_completion_commits_bytes_and_finishes_inline) {
  bnio::io_context context;
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());

  fake_stream_type stream{fake_synchronous_transport{}, ssl_context};
  ASSERT_TRUE(stream.valid());
  constexpr std::string_view payload = "synchronous-handshake-bytes";
  stream.next_layer().inbound = payload;

  wait_observation observation;
  auto sender = bnio::ssl::make_handshake_wait_sender(
      scheduler, stream,
      make_handshake_pause_error(handshake_pause_reason::want_read));
  auto operation =
      bexec::connect(std::move(sender), wait_receiver{&observation});
  bexec::start(operation);

  // The transport completed inside start(), so no io_context::run() is
  // needed; the read BIO received everything in one commit.
  EXPECT_EQ(observation.terminals, 1U);
  EXPECT_EQ(observation.values, 1U);
  EXPECT_EQ(observation.stopped, 0U);
  EXPECT_FALSE(observation.error);

  std::array<char, 64> received{};
  SSL* const ssl = stream.native_handle().native_handle();
  ASSERT_NE(ssl, nullptr);
  const int read = BIO_read(SSL_get_rbio(ssl), received.data(),
                            static_cast<int>(received.size()));
  ASSERT_EQ(read, static_cast<int>(payload.size()));
  EXPECT_EQ(std::string_view(received.data(), static_cast<std::size_t>(read)),
            payload);
}

TEST(SslHandshakeWaitTest,
     synchronous_short_writes_drain_the_write_bio_without_recursion) {
  bnio::io_context context;
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());
  ssl_context.set_verify_mode(SSL_VERIFY_NONE);

  fake_stream_type stream{fake_synchronous_transport{}, ssl_context};
  ASSERT_TRUE(stream.valid());

  // Queue a ClientHello in the write BIO, then let the fake transport accept
  // one byte per synchronous completion: the wait driver must trampoline
  // through the remaining rounds instead of recursing or stranding output.
  SSL* const ssl = stream.native_handle().native_handle();
  ASSERT_NE(ssl, nullptr);
  SSL_set_connect_state(ssl);
  ERR_clear_error();
  const int handshake_result = SSL_do_handshake(ssl);
  ASSERT_EQ(handshake_result, -1);
  EXPECT_EQ(SSL_get_error(ssl, handshake_result), SSL_ERROR_WANT_READ);
  const std::size_t pending = BIO_ctrl_pending(stream.native_write_bio());
  ASSERT_GT(pending, std::size_t{1});
  stream.next_layer().write_chunk = 1;

  wait_observation observation;
  auto sender = bnio::ssl::make_handshake_wait_sender(
      scheduler, stream,
      make_handshake_pause_error(handshake_pause_reason::want_write));
  auto operation =
      bexec::connect(std::move(sender), wait_receiver{&observation});
  bexec::start(operation);

  EXPECT_EQ(observation.terminals, 1U);
  EXPECT_EQ(observation.values, 1U);
  EXPECT_EQ(observation.stopped, 0U);
  EXPECT_FALSE(observation.error);
  EXPECT_EQ(BIO_ctrl_pending(stream.native_write_bio()), std::size_t{0});
  EXPECT_EQ(stream.next_layer().captured.size(), pending);
}

TEST(SslHandshakeWaitTest,
     full_read_bio_reports_no_ssl_error_without_starting_transport) {
  bnio::io_context context;
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());

  socketpair_holder sockets;
  ASSERT_TRUE(sockets.valid());
  stream_type stream{bnio::local::stream_socket(sockets.release_stream_end()),
                     ssl_context};
  ASSERT_TRUE(stream.valid());

  // Fill the transport-side read BIO completely: BIO_nwrite0 cannot offer a
  // staging buffer, so the helper must report last_error() (the dedicated
  // no-OpenSSL-error value, since the queue is clear) without a transport op.
  const std::string fill(bnio::ssl::base::bio_buffer_capacity, 'x');
  ASSERT_EQ(BIO_write(stream.native_read_bio(), fill.data(),
                      static_cast<int>(fill.size())),
            static_cast<int>(fill.size()));

  wait_observation observation;
  auto sender = bnio::ssl::make_handshake_wait_sender(
      scheduler, stream,
      make_handshake_pause_error(handshake_pause_reason::want_read));
  auto operation =
      bexec::connect(std::move(sender), wait_receiver{&observation});
  bexec::start(operation);

  EXPECT_EQ(observation.terminals, 1U);
  EXPECT_EQ(observation.values, 1U);
  EXPECT_EQ(observation.stopped, 0U);
  EXPECT_EQ(observation.error, bnio::ssl::make_no_ssl_error());
}

TEST(SslHandshakeWaitTest,
     over_reported_read_completion_reports_bio_commit_failure) {
  bnio::io_context context;
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());

  const std::string inbound(bnio::ssl::base::bio_buffer_capacity, 'y');
  fake_stream_type stream{fake_synchronous_transport{}, ssl_context};
  ASSERT_TRUE(stream.valid());
  // Claim one byte more than the staging buffer can hold: the BIO commit
  // returns short and the helper reports the invariant failure through the
  // value channel.
  stream.next_layer().inbound = inbound;
  stream.next_layer().read_reported_extra = 1;

  wait_observation observation;
  auto sender = bnio::ssl::make_handshake_wait_sender(
      scheduler, stream,
      make_handshake_pause_error(handshake_pause_reason::want_read));
  auto operation =
      bexec::connect(std::move(sender), wait_receiver{&observation});
  bexec::start(operation);

  EXPECT_EQ(observation.terminals, 1U);
  EXPECT_EQ(observation.values, 1U);
  EXPECT_EQ(observation.stopped, 0U);
  EXPECT_EQ(observation.error, bnio::ssl::make_no_ssl_error());
  // The mismatched bytes were not committed to the read BIO.
  EXPECT_EQ(BIO_ctrl_pending(stream.native_read_bio()), std::size_t{0});
}

TEST(SslHandshakeWaitTest,
     over_reported_write_completion_reports_bio_consume_failure) {
  bnio::io_context context;
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());
  ssl_context.set_verify_mode(SSL_VERIFY_NONE);

  fake_stream_type stream{fake_synchronous_transport{}, ssl_context};
  ASSERT_TRUE(stream.valid());

  SSL* const ssl = stream.native_handle().native_handle();
  ASSERT_NE(ssl, nullptr);
  SSL_set_connect_state(ssl);
  ERR_clear_error();
  const int handshake_result = SSL_do_handshake(ssl);
  ASSERT_EQ(handshake_result, -1);
  ASSERT_EQ(SSL_get_error(ssl, handshake_result), SSL_ERROR_WANT_READ);
  ASSERT_GT(BIO_ctrl_pending(stream.native_write_bio()), std::size_t{0});

  // Claim one byte more than the BIO exposes: the consume returns short and
  // the helper reports the invariant failure through the value channel.
  stream.next_layer().write_reported_extra = 1;

  wait_observation observation;
  auto sender = bnio::ssl::make_handshake_wait_sender(
      scheduler, stream,
      make_handshake_pause_error(handshake_pause_reason::want_write));
  auto operation =
      bexec::connect(std::move(sender), wait_receiver{&observation});
  bexec::start(operation);

  EXPECT_EQ(observation.terminals, 1U);
  EXPECT_EQ(observation.values, 1U);
  EXPECT_EQ(observation.stopped, 0U);
  EXPECT_EQ(observation.error, bnio::ssl::make_no_ssl_error());
}

TEST(SslHandshakeWaitTest, want_write_transport_error_is_reported) {
  bnio::io_context context;
  if (!context.is_open()) {
    return;
  }
  auto scheduler = context.get_post_scheduler();
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());
  ssl_context.set_verify_mode(SSL_VERIFY_NONE);

  socketpair_holder sockets;
  ASSERT_TRUE(sockets.valid());
  stream_type stream{bnio::local::stream_socket(sockets.release_stream_end()),
                     ssl_context};
  ASSERT_TRUE(stream.valid());

  SSL* const ssl = stream.native_handle().native_handle();
  ASSERT_NE(ssl, nullptr);
  SSL_set_connect_state(ssl);
  ERR_clear_error();
  const int handshake_result = SSL_do_handshake(ssl);
  ASSERT_EQ(handshake_result, -1);
  ASSERT_EQ(SSL_get_error(ssl, handshake_result), SSL_ERROR_WANT_READ);
  ASSERT_GT(BIO_ctrl_pending(stream.native_write_bio()), std::size_t{0});

  // The transport is gone before the flush: the write completes with a
  // transport error through the value channel (MSG_NOSIGNAL keeps this from
  // raising SIGPIPE).
  sockets.close_peer();

  wait_observation observation;
  auto sender = bnio::ssl::make_handshake_wait_sender(
      scheduler, stream,
      make_handshake_pause_error(handshake_pause_reason::want_write));
  auto operation =
      bexec::connect(std::move(sender), wait_receiver{&observation, &context});
  bexec::start(operation);
  context.run();

  EXPECT_EQ(observation.terminals, 1U);
  EXPECT_EQ(observation.values, 1U);
  EXPECT_EQ(observation.stopped, 0U);
  EXPECT_TRUE(observation.error);
  EXPECT_FALSE(is_handshake_pause(observation.error));
}

}  // namespace
