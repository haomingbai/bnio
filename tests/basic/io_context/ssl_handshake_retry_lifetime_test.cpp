/**
 * @file ssl_handshake_retry_lifetime_test.cpp
 * @brief Lifetime regression for terminal completions of the retry operation.
 *
 * Every transport/completion path that terminalizes the factory-driven
 * handshake may run fully synchronously: the fake transport completes inside
 * start(), the inline scheduler posts inside submit_post(), and the public
 * receiver deletes the operation from inside set_value/set_stopped. The
 * operation must therefore submit its terminal post from the driver loop as
 * the loop's last action; touching any member afterwards is a use-after-free.
 *
 * This target is compiled with AddressSanitizer in CMake so the regression is
 * caught even when it happens to look harmless without instrumentation.
 */

#include <bnio/local.h>
#include <bnio/ssl.h>
#include <gtest/gtest.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <bexec/just.hpp>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cstddef>
#include <memory>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using bnio::ssl::handshake_type;

// Completes posts synchronously: the schedule post runs inside submit_post(),
// which is exactly the review scenario where the public receiver may destroy
// the operation before the driver loop unwinds.
struct inline_scheduler {
  bool operator==(const inline_scheduler&) const = default;

  [[nodiscard]] auto schedule() const { return bexec::just(std::error_code{}); }
};

// Transport step completing synchronously with a caller-chosen result.
struct sync_io_sender {
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  std::error_code error;
  std::size_t bytes = 0;

  template <class Receiver>
  struct operation {
    Receiver receiver;
    std::error_code error;
    std::size_t bytes = 0;

    void start() noexcept {
      bexec::set_value(std::move(receiver), error, bytes);
    }
  };

  template <class Receiver>
  operation<Receiver> connect(Receiver receiver) && {
    return operation<Receiver>{std::move(receiver), error, bytes};
  }
};

// Fake next layer: every transport step completes synchronously. A zero-sized
// write with an empty error reports EOF; reported_extra breaks the transport
// contract to reach the BIO invariant paths.
struct sync_transport {
  std::error_code write_error;
  std::size_t write_bytes = 0;
  std::size_t write_reported_extra = 0;
  bool accept_all_writes = false;

  sync_transport& lowest_layer() noexcept { return *this; }
  const sync_transport& lowest_layer() const noexcept { return *this; }

  template <class Scheduler>
  auto async_write_some(Scheduler&, bnio::const_buffer buffer, int) {
    std::size_t accepted = write_bytes;
    if (accept_all_writes) {
      accepted = buffer.size();
    }
    return sync_io_sender{write_error, accepted + write_reported_extra};
  }

  template <class Scheduler>
  auto async_read_some(Scheduler&, bnio::mutable_buffer) {
    // Zero bytes with an empty error is transport EOF.
    return sync_io_sender{};
  }
};

using stream_type = bnio::ssl::tcp::stream<sync_transport>;
using scheduler_type = inline_scheduler;

struct completion_record {
  unsigned values = 0;
  unsigned errors = 0;
  unsigned stopped = 0;
  std::error_code last_error;
  bool saw_pause = false;
};

// Type-erased deleter for the operation type, which the receiver must not
// need to name.
struct operation_deleter {
  void (*destroy)(void*) = nullptr;

  void operator()(void* pointer) const noexcept {
    if (pointer != nullptr && destroy != nullptr) {
      destroy(pointer);
    }
  }
};

struct victim_receiver {
  completion_record* record = nullptr;
  std::unique_ptr<void, operation_deleter>* owner = nullptr;

  void set_value(std::error_code ec) noexcept {
    record->last_error = ec;
    if (bnio::ssl::is_handshake_pause(ec)) {
      record->saw_pause = true;
    }
    if (ec) {
      ++record->errors;
    } else {
      ++record->values;
    }
    owner->reset();
  }

  void set_stopped() noexcept {
    ++record->stopped;
    owner->reset();
  }
};

struct terminal_factory {
  std::error_code terminal;

  auto operator()(const std::error_code&) const {
    return bexec::just(terminal);
  }
};

// Advertises the full retry contract but completes with set_stopped()
// synchronously. bexec::just_stopped() alone is rejected by the factory
// concept (it advertises no value signature).
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

struct stopped_factory {
  auto operator()(const std::error_code&) const {
    return sync_stopped_sender{};
  }
};

// Completes synchronously with an empty error: the retry operation resumes the
// SSL attempt after the callback pause.
struct resume_factory {
  auto operator()(const std::error_code&) const {
    return bexec::just(std::error_code{});
  }
};

// A client SSL object produces a real ClientHello into its write BIO. The
// bytes are later preloaded into a server stream's read BIO so the
// client_hello callback is reached deterministically.
using generator_stream_type =
    bnio::ssl::tcp::stream<bnio::local::stream_socket>;

[[nodiscard]] std::vector<char> make_client_hello() {
  bnio::ssl::context client_context(bnio::ssl::context_method::tls_client);
  EXPECT_TRUE(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  generator_stream_type generator{bnio::local::stream_socket(-1),
                                  client_context};
  EXPECT_TRUE(generator.valid());
  SSL* const ssl = generator.native_handle().native_handle();
  EXPECT_NE(ssl, nullptr);
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

void preload_client_hello(stream_type& stream, const std::vector<char>& hello) {
  EXPECT_EQ(BIO_write(stream.native_read_bio(), hello.data(),
                      static_cast<int>(hello.size())),
            static_cast<int>(hello.size()));
}

template <class Factory>
using sender_type = decltype(std::declval<stream_type&>().async_handshake(
    std::declval<scheduler_type>(), std::declval<handshake_type>(),
    std::declval<Factory>()));

template <class Factory>
using operation_type = decltype(bexec::connect(
    std::declval<sender_type<Factory>>(), std::declval<victim_receiver>()));

template <class Operation>
void destroy_operation(void* pointer) noexcept {
  delete static_cast<Operation*>(pointer);
}

class SslHandshakeRetryLifetimeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    client_context_ = std::make_unique<bnio::ssl::context>(
        bnio::ssl::context_method::tls_client);
    ASSERT_TRUE(client_context_->valid());
    client_context_->set_verify_mode(SSL_VERIFY_NONE);

    server_context_ = std::make_unique<bnio::ssl::context>(
        bnio::ssl::context_method::tls_server);
    ASSERT_TRUE(server_context_->valid());
    // The retry operation drives transport pauses internally, so a server
    // test that needs the factory path must pause in a callback. This always
    // retrying callback provides that pause once a ClientHello is preloaded.
    server_context_->set_client_hello_callback(
        [](bnio::ssl::context_view, bnio::ssl::stream_view,
           bnio::ssl::alert_out&) -> bnio::ssl::client_hello_result {
          return bnio::ssl::client_hello_result::retry;
        });
  }

  // Starts the retry operation over the fake transport with a receiver that
  // deletes the operation as soon as it completes. The factory builder sees
  // the actual stream the operation drives.
  template <class FactoryBuilder>
  [[nodiscard]] bool run_and_destroy(sync_transport transport,
                                     handshake_type type,
                                     FactoryBuilder build_factory,
                                     completion_record& record) {
    bnio::ssl::context& ssl_context =
        type == handshake_type::server ? *server_context_ : *client_context_;
    stream_type stream{std::move(transport), ssl_context};
    EXPECT_TRUE(stream.valid());

    using factory_type = std::remove_cvref_t<decltype(build_factory(stream))>;
    using operation_t = operation_type<factory_type>;
    using handle_t = std::unique_ptr<void, operation_deleter>;

    handle_t owner{nullptr, operation_deleter{&destroy_operation<operation_t>}};

    auto sender =
        stream.async_handshake(scheduler_type{}, type, build_factory(stream));
    auto* operation = new operation_t(
        bexec::connect(std::move(sender), victim_receiver{&record, &owner}));
    owner.reset(operation);

    bexec::start(*operation);

    // The receiver deletes the operation from inside the inline completion;
    // the owner is empty only when that deletion happened exactly once.
    return owner == nullptr;
  }

  std::unique_ptr<bnio::ssl::context> client_context_;
  std::unique_ptr<bnio::ssl::context> server_context_;
};

TEST_F(SslHandshakeRetryLifetimeTest,
       inline_transport_error_terminal_does_not_touch_the_operation) {
  completion_record record;
  sync_transport transport;
  transport.write_error = std::make_error_code(std::errc::io_error);
  transport.write_bytes = 5;

  // A client queues its ClientHello, so the pause flush immediately starts a
  // transport write that completes synchronously with a transport error.
  ASSERT_TRUE(run_and_destroy(
      std::move(transport), handshake_type::client,
      [](stream_type&) { return terminal_factory{}; }, record));

  EXPECT_EQ(record.errors, 1U);
  EXPECT_EQ(record.values, 0U);
  EXPECT_EQ(record.stopped, 0U);
  EXPECT_EQ(record.last_error, std::make_error_code(std::errc::io_error));
  EXPECT_FALSE(record.saw_pause);
}

TEST_F(SslHandshakeRetryLifetimeTest,
       inline_transport_eof_terminal_does_not_touch_the_operation) {
  completion_record record;
  sync_transport transport;
  // A zero-byte write with an empty error is transport EOF.
  transport.write_bytes = 0;

  ASSERT_TRUE(run_and_destroy(
      std::move(transport), handshake_type::client,
      [](stream_type&) { return terminal_factory{}; }, record));

  EXPECT_EQ(record.errors, 1U);
  EXPECT_EQ(record.values, 0U);
  EXPECT_EQ(record.stopped, 0U);
  EXPECT_EQ(record.last_error,
            std::make_error_code(std::errc::connection_reset));
}

TEST_F(SslHandshakeRetryLifetimeTest,
       inline_bio_invariant_terminal_does_not_touch_the_operation) {
  completion_record record;
  sync_transport transport;
  // Report one byte more than the transport can have accepted: the write BIO
  // consume returns short and the invariant path terminalizes the operation.
  transport.accept_all_writes = true;
  transport.write_reported_extra = 1;

  ASSERT_TRUE(run_and_destroy(
      std::move(transport), handshake_type::client,
      [](stream_type&) { return terminal_factory{}; }, record));

  EXPECT_EQ(record.errors, 1U);
  EXPECT_EQ(record.values, 0U);
  EXPECT_EQ(record.stopped, 0U);
  EXPECT_EQ(record.last_error, bnio::ssl::make_no_ssl_error());
}

TEST_F(SslHandshakeRetryLifetimeTest,
       inline_child_stopped_terminal_does_not_touch_the_operation) {
  completion_record record;
  const std::vector<char> hello = make_client_hello();
  // A server with a preloaded ClientHello pauses in the client_hello callback
  // without producing output; the factory's synchronous set_stopped()
  // terminalizes the operation.
  ASSERT_TRUE(run_and_destroy(
      sync_transport{}, handshake_type::server,
      [hello](stream_type& stream) {
        preload_client_hello(stream, hello);
        return stopped_factory{};
      },
      record));

  EXPECT_EQ(record.errors, 0U);
  EXPECT_EQ(record.values, 0U);
  EXPECT_EQ(record.stopped, 1U);
}

TEST_F(SslHandshakeRetryLifetimeTest,
       inline_factory_terminal_value_does_not_touch_the_operation) {
  completion_record record;
  const std::error_code terminal =
      std::make_error_code(std::errc::permission_denied);
  const std::vector<char> hello = make_client_hello();

  ASSERT_TRUE(run_and_destroy(
      sync_transport{}, handshake_type::server,
      [hello, terminal](stream_type& stream) {
        preload_client_hello(stream, hello);
        return terminal_factory{terminal};
      },
      record));

  EXPECT_EQ(record.errors, 1U);
  EXPECT_EQ(record.values, 0U);
  EXPECT_EQ(record.stopped, 0U);
  EXPECT_EQ(record.last_error, terminal);
}

TEST_F(SslHandshakeRetryLifetimeTest,
       inline_callback_resume_terminal_does_not_touch_the_operation) {
  completion_record record;
  sync_transport transport;
  // Accept whatever the resumed attempt produces so its flush completes
  // synchronously.
  transport.accept_all_writes = true;

  // The callback retries once so the factory runs, then succeeds so the
  // factory's empty result resumes the handshake. The resumed attempt
  // terminalizes synchronously: a fatal SSL error for this certificate-less
  // server, or transport EOF if OpenSSL asks for more bytes. This is the
  // callback-pause replacement for the old wait-step resume path.
  unsigned callback_calls = 0;
  server_context_->set_client_hello_callback(
      [](bnio::ssl::context_view, bnio::ssl::stream_view, bnio::ssl::alert_out&,
         unsigned* calls) -> bnio::ssl::client_hello_result {
        return ++*calls == 1 ? bnio::ssl::client_hello_result::retry
                             : bnio::ssl::client_hello_result::success;
      },
      &callback_calls);
  const std::vector<char> hello = make_client_hello();

  ASSERT_TRUE(run_and_destroy(
      std::move(transport), handshake_type::server,
      [hello](stream_type& stream) {
        preload_client_hello(stream, hello);
        return resume_factory{};
      },
      record));

  EXPECT_GE(callback_calls, 2U);
  EXPECT_EQ(record.errors, 1U);
  EXPECT_EQ(record.values, 0U);
  EXPECT_EQ(record.stopped, 0U);
  EXPECT_TRUE(record.last_error);
  EXPECT_FALSE(record.saw_pause);
}

TEST_F(SslHandshakeRetryLifetimeTest,
       inline_transport_read_eof_terminal_does_not_touch_the_operation) {
  completion_record record;
  sync_transport transport;
  // Accept the ClientHello so its pause flush succeeds; the internal
  // transport read that follows then observes EOF and reports
  // connection_reset. This is the callback-free counterpart of the old
  // factory wait-step path: want_read is now driven inside the operation.
  transport.accept_all_writes = true;

  ASSERT_TRUE(run_and_destroy(
      std::move(transport), handshake_type::client,
      [](stream_type&) { return terminal_factory{}; }, record));

  EXPECT_EQ(record.errors, 1U);
  EXPECT_EQ(record.values, 0U);
  EXPECT_EQ(record.stopped, 0U);
  EXPECT_EQ(record.last_error,
            std::make_error_code(std::errc::connection_reset));
}

}  // namespace
