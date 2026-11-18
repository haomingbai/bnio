/**
 * @file ssl_handshake_retry_transport_test.cpp
 * @brief Deterministic transport-plumbing coverage for the retry handshake.
 *
 * The retry operation drives transport pauses (SSL_ERROR_WANT_READ and
 * SSL_ERROR_WANT_WRITE) through operation_base: it flushes pending output,
 * performs the requested transport step, and retries the SSL attempt. These
 * tests use a synchronous fake next layer so every BIO flush/read and
 * transport completion runs inside start(), without an io_context. They cover
 * the paths that used to be exercised by the removed handshake wait helper:
 * short synchronous writes, an inline read completion carrying payload bytes,
 * a full read BIO, and an over-reported read completion.
 */

#include <bnio/ssl.h>
#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <array>
#include <bexec/completion_signatures.hpp>
#include <bexec/just.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using bnio::ssl::handshake_type;

// Completes schedule() synchronously: the retry operation's terminal post is
// delivered inside start().
struct inline_scheduler {
  bool operator==(const inline_scheduler&) const = default;

  [[nodiscard]] auto schedule() const { return bexec::just(std::error_code{}); }
};

// Transport read completing synchronously. The payload is copied into the
// staging buffer obtained from the read BIO; reported_extra breaks the
// transport contract on purpose to reach the BIO commit invariant.
struct fake_read_sender {
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  bnio::mutable_buffer buffer;
  const char* data = nullptr;
  std::size_t size = 0;
  std::size_t reported_extra = 0;

  template <class Receiver>
  struct operation {
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

  template <class Receiver>
  operation<Receiver> connect(Receiver receiver) && {
    return operation<Receiver>{std::move(receiver), buffer, data, size,
                               reported_extra};
  }
};

// Transport write completing synchronously. chunk limits how many bytes one
// completion accepts, which drives the write-BIO drain through several rounds;
// reported_extra breaks the transport contract on purpose.
struct fake_write_sender {
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  bnio::const_buffer buffer;
  std::string* captured = nullptr;
  std::size_t chunk = 0;
  std::size_t reported_extra = 0;
  std::size_t* calls = nullptr;

  template <class Receiver>
  struct operation {
    Receiver receiver;
    bnio::const_buffer buffer;
    std::string* captured = nullptr;
    std::size_t chunk = 0;
    std::size_t reported_extra = 0;
    std::size_t* calls = nullptr;

    void start() noexcept {
      ++*calls;
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

  template <class Receiver>
  operation<Receiver> connect(Receiver receiver) && {
    return operation<Receiver>{std::move(receiver), buffer, captured, chunk,
                               reported_extra,      calls};
  }
};

// Minimal synchronous next layer: every transport step completes from
// start(). fill_read_bio fills the transport-side read BIO during the first
// output write, which happens after the SSL attempt asked for input and before
// the internal transport read is staged.
struct sync_transport {
  std::string inbound;
  std::string captured;
  std::size_t write_chunk = 0;
  std::size_t read_reported_extra = 0;
  std::size_t write_reported_extra = 0;
  std::size_t read_calls = 0;
  std::size_t write_calls = 0;
  BIO* fill_read_bio = nullptr;

  sync_transport& lowest_layer() noexcept { return *this; }
  const sync_transport& lowest_layer() const noexcept { return *this; }

  template <class Scheduler>
  auto async_read_some(Scheduler&, bnio::mutable_buffer buffer) {
    ++read_calls;
    return fake_read_sender{buffer, inbound.data(), inbound.size(),
                            read_reported_extra};
  }

  template <class Scheduler>
  auto async_write_some(Scheduler&, bnio::const_buffer buffer, int) {
    if (fill_read_bio != nullptr) {
      fill_read_bio_once();
    }
    return fake_write_sender{buffer, &captured, write_chunk,
                             write_reported_extra, &write_calls};
  }

 private:
  void fill_read_bio_once() {
    BIO* const bio = fill_read_bio;
    fill_read_bio = nullptr;
    const std::string fill(bnio::ssl::base::bio_buffer_capacity, 'x');
    EXPECT_EQ(BIO_write(bio, fill.data(), static_cast<int>(fill.size())),
              static_cast<int>(fill.size()));
  }
};

using stream_type = bnio::ssl::tcp::stream<sync_transport>;

struct observation {
  unsigned values = 0;
  unsigned errors = 0;
  unsigned stopped = 0;
  std::error_code error;
};

struct sync_receiver {
  observation* out = nullptr;

  void set_value(std::error_code ec) noexcept {
    out->error = ec;
    if (ec) {
      ++out->errors;
    } else {
      ++out->values;
    }
  }

  void set_stopped() noexcept { ++out->stopped; }
};

// Runs the 2-argument handshake to completion on the synchronous fake
// transport. The default factory cannot resume a callback-driven pause, so
// these tests exercise transport pauses only.
void run_client_handshake(stream_type& stream, observation& observed) {
  auto sender =
      stream.async_handshake(inline_scheduler{}, handshake_type::client);
  auto operation = bexec::connect(std::move(sender), sync_receiver{&observed});
  bexec::start(operation);
}

TEST(SslHandshakeRetryTransportTest,
     short_synchronous_writes_drain_the_write_bio_without_stranding_output) {
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());
  ssl_context.set_verify_mode(SSL_VERIFY_NONE);

  sync_transport transport;
  // Accept one byte per synchronous completion: the flush must drain the
  // remaining rounds through re-entrant child replacement without stranding
  // output or corrupting the BIO.
  transport.write_chunk = 1;
  stream_type stream{std::move(transport), ssl_context};
  ASSERT_TRUE(stream.valid());

  observation observed;
  run_client_handshake(stream, observed);

  const sync_transport& used = stream.next_layer();
  EXPECT_EQ(observed.errors, 1u);
  EXPECT_EQ(observed.values, 0u);
  EXPECT_EQ(observed.stopped, 0u);
  // The ClientHello was drained and the internal read then observed EOF.
  EXPECT_EQ(observed.error, std::make_error_code(std::errc::connection_reset));
  EXPECT_GT(used.captured.size(), std::size_t{1});
  EXPECT_EQ(used.write_calls, used.captured.size());
  EXPECT_EQ(BIO_ctrl_pending(stream.native_write_bio()), std::size_t{0});
}

TEST(SslHandshakeRetryTransportTest,
     want_write_flushes_the_full_write_bio_before_retrying) {
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());
  ssl_context.set_verify_mode(SSL_VERIFY_NONE);

  sync_transport transport;
  stream_type stream{std::move(transport), ssl_context};
  ASSERT_TRUE(stream.valid());

  // Fill the SSL-side write half to capacity. The first attempt cannot queue
  // the ClientHello, so SSL_do_handshake reports SSL_ERROR_WANT_WRITE; the
  // retry operation must flush the pending output before resuming the
  // attempt.
  SSL* const ssl = stream.native_handle().native_handle();
  ASSERT_NE(ssl, nullptr);
  std::string filler(bnio::ssl::base::bio_buffer_capacity, 'z');
  ASSERT_EQ(BIO_write(SSL_get_wbio(ssl), filler.data(),
                      static_cast<int>(filler.size())),
            static_cast<int>(filler.size()));

  observation observed;
  run_client_handshake(stream, observed);

  const sync_transport& used = stream.next_layer();
  EXPECT_EQ(observed.errors, 1u);
  EXPECT_EQ(observed.values, 0u);
  EXPECT_EQ(observed.stopped, 0u);
  EXPECT_EQ(observed.error, std::make_error_code(std::errc::connection_reset));
  // The want_write flush drained the pre-filled output, and the retried
  // attempt then queued the ClientHello.
  EXPECT_GT(used.write_calls, std::size_t{0});
  EXPECT_GT(used.captured.size(), filler.size());
  EXPECT_EQ(BIO_ctrl_pending(stream.native_write_bio()), std::size_t{0});
}

TEST(SslHandshakeRetryTransportTest,
     inline_read_completion_commits_payload_before_resuming) {
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());
  ssl_context.set_verify_mode(SSL_VERIFY_NONE);
  ASSERT_FALSE(ssl_context.set_max_proto_version(TLS1_2_VERSION));

  // A plaintext close_notify alert is accepted before the handshake keys are
  // established; the trailing bytes are not consumed by the alert and must
  // remain committed in the read BIO, proving the inline completion's whole
  // payload was committed before the attempt resumed.
  constexpr char close_notify[] = {0x15, 0x03, 0x03, 0x00, 0x02, 0x01, 0x00};
  constexpr std::string_view trailing = "retained";
  sync_transport transport;
  transport.inbound.assign(close_notify, sizeof(close_notify));
  transport.inbound.append(trailing.data(), trailing.size());
  stream_type stream{std::move(transport), ssl_context};
  ASSERT_TRUE(stream.valid());

  observation observed;
  run_client_handshake(stream, observed);

  EXPECT_EQ(observed.errors, 1u);
  EXPECT_EQ(observed.values, 0u);
  EXPECT_EQ(observed.stopped, 0u);
  EXPECT_EQ(observed.error, std::make_error_code(std::errc::connection_reset));
  EXPECT_EQ(stream.next_layer().read_calls, 1u);

  SSL* const ssl = stream.native_handle().native_handle();
  ASSERT_NE(ssl, nullptr);
  BIO* const ssl_read = SSL_get_rbio(ssl);
  ASSERT_NE(ssl_read, nullptr);
  std::array<char, 16> received{};
  const int read =
      BIO_read(ssl_read, received.data(), static_cast<int>(received.size()));
  ASSERT_EQ(read, static_cast<int>(trailing.size()));
  EXPECT_EQ(std::string_view(received.data(), static_cast<std::size_t>(read)),
            trailing);
}

TEST(SslHandshakeRetryTransportTest,
     full_read_bio_reports_no_ssl_error_without_starting_transport) {
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());
  ssl_context.set_verify_mode(SSL_VERIFY_NONE);

  sync_transport transport;
  stream_type stream{std::move(transport), ssl_context};
  ASSERT_TRUE(stream.valid());
  // The fill happens while the ClientHello flush is starting: the SSL attempt
  // has already asked for input, so the following internal read finds no
  // staging space and must report the dedicated no-OpenSSL-error value
  // without starting a transport read.
  stream.next_layer().fill_read_bio = stream.native_read_bio();

  observation observed;
  run_client_handshake(stream, observed);

  EXPECT_EQ(observed.values, 0u);
  EXPECT_EQ(observed.errors, 1u);
  EXPECT_EQ(observed.stopped, 0u);
  EXPECT_EQ(observed.error, bnio::ssl::make_no_ssl_error());
  EXPECT_GT(stream.next_layer().write_calls, std::size_t{0});
  EXPECT_EQ(stream.next_layer().read_calls, 0u);
  EXPECT_EQ(BIO_ctrl_pending(stream.native_write_bio()), std::size_t{0});
}

TEST(SslHandshakeRetryTransportTest,
     over_reported_read_completion_reports_bio_commit_failure) {
  bnio::ssl::context ssl_context(bnio::ssl::context_method::tls_client);
  ASSERT_TRUE(ssl_context.valid());
  ssl_context.set_verify_mode(SSL_VERIFY_NONE);

  sync_transport transport;
  // Fill the whole staging buffer and claim one byte more: the BIO commit
  // returns short of the reported count, so the operation must terminalize
  // through the value channel instead of resuming the handshake.
  transport.inbound.assign(bnio::ssl::base::bio_buffer_capacity, 'y');
  transport.read_reported_extra = 1;
  stream_type stream{std::move(transport), ssl_context};
  ASSERT_TRUE(stream.valid());

  observation observed;
  run_client_handshake(stream, observed);

  EXPECT_EQ(observed.values, 0u);
  EXPECT_EQ(observed.errors, 1u);
  EXPECT_EQ(observed.stopped, 0u);
  EXPECT_EQ(observed.error, bnio::ssl::make_no_ssl_error());
  EXPECT_EQ(stream.next_layer().read_calls, 1u);
}

}  // namespace
