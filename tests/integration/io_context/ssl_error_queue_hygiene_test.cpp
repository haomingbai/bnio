#include <gtest/gtest.h>
#include <openssl/err.h>

#include <thread>

#include "../../support/io_context/ssl_test_support.h"

namespace {

// The value bnio reports when a failure path completed without any OpenSSL
// call recording an error: -1 in the OpenSSL error category. Real OpenSSL
// error codes are non-negative (ERR_get_error() returns 0 only for an empty
// queue), so -1 can never collide with a genuine error code.
constexpr int k_no_ssl_error_value = -1;

// The invalid-stream handshake never reaches OpenSSL. Its reported ec must
// therefore not depend on the calling thread's thread-local OpenSSL error
// queue: a stale entry seeded by earlier work must not leak into the result,
// and an empty queue must not be papered over with a fabricated
// protocol_error. Both are reported as the dedicated no-OpenSSL-error value.
TEST(SslErrorQueueHygieneTest,
     stale_err_queue_entry_does_not_leak_into_ssl_failure) {
  bnio::io_context context;
  if (!context.is_open()) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  bnio::ssl_context ssl_context(bnio::ssl_context_method::tls_client);
  test_require(ssl_context.valid());
  bnio::ssl_stream source{bnio::tcp_socket(-1), ssl_context};
  bnio::ssl_stream owner{std::move(source)};
  EXPECT_FALSE(source.valid());
  EXPECT_TRUE(owner.valid());

  ERR_clear_error();
  ERR_put_error(ERR_LIB_USER, 0, 1234, "bnio_test.c", 1);
  const unsigned long seeded = ERR_peek_error();
  test_require(seeded != 0);
  const std::error_code stale = bnio::make_openssl_error(seeded);

  auto state = std::make_shared<handshake_state>();
  auto sender =
      source.async_handshake(scheduler, bnio::ssl_handshake_type::client);
  auto operation =
      bexec::connect(std::move(sender), handshake_receiver{state, &context});
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->values, 0);
  EXPECT_EQ(state->errors, 1);
  EXPECT_EQ(state->stopped, 0);
  // Neither the stale seeded entry ...
  EXPECT_NE(state->error, stale);
  // ... nor a fabricated protocol_error impersonating a TLS failure ...
  EXPECT_FALSE(state->error == std::errc::protocol_error);
  // ... but the dedicated no-OpenSSL-error value.
  EXPECT_EQ(state->error, std::error_code(k_no_ssl_error_value,
                                          bnio::openssl_error_category()));
}

// The same SSL failure must report the same ec regardless of which worker
// thread runs it. Each fresh thread runs the identical invalid-stream
// handshake against its own io_context (no shared objects); one thread seeds
// a stale entry into its error queue first, the other clears its queue.
// Before the fix the seeding thread surfaced the stale entry while the
// cleared thread surfaced a fabricated protocol_error, so the same failure
// produced two different error codes depending on thread-local history.
TEST(SslErrorQueueHygieneTest,
     same_ssl_failure_reports_same_ec_on_fresh_threads) {
  auto run_failure = [](bool seed, std::error_code* out) {
    bnio::io_context context;
    if (!context.is_open()) {
      return;
    }
    auto scheduler = context.get_post_scheduler();

    bnio::ssl_context ssl_context(bnio::ssl_context_method::tls_client);
    test_require(ssl_context.valid());
    bnio::ssl_stream source{bnio::tcp_socket(-1), ssl_context};
    bnio::ssl_stream owner{std::move(source)};

    if (seed) {
      ERR_put_error(ERR_LIB_USER, 0, 1234, "bnio_test.c", 1);
    } else {
      ERR_clear_error();
    }

    auto state = std::make_shared<handshake_state>();
    auto sender =
        source.async_handshake(scheduler, bnio::ssl_handshake_type::client);
    auto operation =
        bexec::connect(std::move(sender), handshake_receiver{state, &context});
    bexec::start(operation);
    context.run();
    *out = state->error;
  };

  std::error_code error_a;
  std::error_code error_b;
  std::thread thread_a(run_failure, true, &error_a);
  std::thread thread_b(run_failure, false, &error_b);
  thread_a.join();
  thread_b.join();

  EXPECT_EQ(error_a, error_b);
  EXPECT_EQ(error_a, std::error_code(k_no_ssl_error_value,
                                     bnio::openssl_error_category()));
}

}  // namespace
