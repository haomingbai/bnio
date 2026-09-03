#include <gtest/gtest.h>
#include <openssl/err.h>

#include "../../support/io_context/ssl_test_support.h"

namespace {

// Contract coverage for docs/usage/index.md ("Already-completed results are
// delivered unchanged"): when the SSL state machine has already staged a
// completion (here: a TLS error from the invalid-stream path) and the
// schedule handoff that delivers it runs after io_context::stop(), the post
// step must deliver the staged result unchanged. The scheduler's
// operation_canceled abort ec must never overwrite a staged completion.
//
// Deterministic construction: an io_context with no worker, stopped before
// start(). The invalid-stream path (no OpenSSL call, empty error queue ->
// last_ssl_error() == protocol_error) stages the error and submits the post
// synchronously inside bexec::start(); the already-stopped context rejects
// the post and completes it inline with operation_canceled.
TEST(SslErrorPrecedenceTest, staged_tls_error_survives_post_receiver_cancel) {
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

  ERR_clear_error();
  // Stop the context (which never ran) before starting the operation: the
  // staged error must win over the schedule's operation_canceled abort.
  EXPECT_GE(context.stop(), 0);

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
  // The staged TLS error, not the scheduler's operation_canceled abort.
  EXPECT_TRUE(state->error == std::errc::protocol_error);
  EXPECT_FALSE(state->error == std::errc::operation_canceled);
}

}  // namespace
