#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "../../support/io_context/ssl_test_support.h"

namespace {

TEST(SslTransferTest, socketpair_read_write_transfers_plaintext) {
  test_certificate_files files;

  bnio::ssl_context server_context(bnio::ssl_context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());

  bnio::ssl_context client_context(bnio::ssl_context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);

  bnio::ssl_stream client{bnio::tcp_socket(sockets[0]), client_context};
  bnio::ssl_stream server{bnio::tcp_socket(sockets[1]), server_context};

  {
    bnio::io_context context;
    if (!context.is_open()) {
      return;
    }
    auto scheduler = context.get_post_scheduler();

    auto state = std::make_shared<handshake_state>();
    handshake_receiver client_receiver{state, &context};
    handshake_receiver server_receiver{state, &context};

    auto client_sender =
        client.async_handshake(scheduler, bnio::ssl_handshake_type::client);
    auto server_sender =
        server.async_handshake(scheduler, bnio::ssl_handshake_type::server);

    auto client_operation =
        bexec::connect(std::move(client_sender), std::move(client_receiver));
    auto server_operation =
        bexec::connect(std::move(server_sender), std::move(server_receiver));

    bexec::start(client_operation);
    bexec::start(server_operation);
    context.run();

    EXPECT_EQ(state->values, 2);
    EXPECT_EQ(state->errors, 0);
    EXPECT_EQ(state->stopped, 0);
  }

  constexpr std::size_t payload_size = 8 * 1024;
  std::vector<unsigned char> payload(payload_size);
  for (std::size_t index = 0; index < payload.size(); ++index) {
    payload[index] = static_cast<unsigned char>((index * 17U + 29U) & 0xffU);
  }
  std::vector<unsigned char> received(payload.size());

  std::size_t sent = 0;
  std::size_t received_size = 0;
  while (received_size < payload.size()) {
    bnio::io_context context;
    if (!context.is_open()) {
      return;
    }
    auto scheduler = context.get_post_scheduler();

    unsigned completions = 0;
    auto read_state = std::make_shared<transfer_state>();
    auto write_state = std::make_shared<transfer_state>();
    const unsigned target = sent < payload.size() ? 2U : 1U;
    transfer_receiver read_receiver{read_state, &context, &completions, target};
    transfer_receiver write_receiver{write_state, &context, &completions,
                                     target};

    auto read_buffer = bnio::buffer(received.data() + received_size,
                                    received.size() - received_size);
    auto read_sender = server.async_read(scheduler, read_buffer);

    if (sent < payload.size()) {
      auto write_buffer =
          bnio::buffer(payload.data() + sent, payload.size() - sent);
      auto write_sender =
          client.async_write(scheduler, write_buffer, MSG_NOSIGNAL);

      auto read_operation =
          bexec::connect(std::move(read_sender), std::move(read_receiver));
      auto write_operation =
          bexec::connect(std::move(write_sender), std::move(write_receiver));

      bexec::start(read_operation);
      bexec::start(write_operation);
      if (completions != target) {
        context.run();
      }

      EXPECT_EQ(write_state->values, 1);
      EXPECT_EQ(write_state->errors, 0);
      EXPECT_EQ(write_state->stopped, 0);
      EXPECT_EQ(write_state->bytes, payload.size() - sent);
      sent += write_state->bytes;
    } else {
      auto read_operation =
          bexec::connect(std::move(read_sender), std::move(read_receiver));

      bexec::start(read_operation);
      if (completions != target) {
        context.run();
      }
    }

    EXPECT_EQ(completions, target);
    EXPECT_EQ(read_state->values, 1);
    EXPECT_EQ(read_state->errors, 0);
    EXPECT_EQ(read_state->stopped, 0);
    EXPECT_NE(read_state->bytes, 0);

    received_size += read_state->bytes;
  }

  EXPECT_EQ(sent, payload.size());
  EXPECT_EQ(received_size, payload.size());
  EXPECT_TRUE(std::memcmp(received.data(), payload.data(), payload.size()) ==
              0);

  test_require(!client.lowest_layer().close());
  bnio::io_context error_context;
  if (!error_context.is_open()) {
    return;
  }
  auto error_scheduler = error_context.get_post_scheduler();
  std::array<unsigned char, 1> trailing_byte{};
  unsigned completions = 0;
  auto error_state = std::make_shared<transfer_state>();
  auto read_operation = bexec::connect(
      server.async_read(error_scheduler, trailing_byte),
      transfer_receiver{error_state, &error_context, &completions, 1});
  bexec::start(read_operation);
  error_context.run();

  EXPECT_EQ(completions, 1);
  EXPECT_EQ(error_state->values, 0);
  EXPECT_EQ(error_state->errors, 1);
  EXPECT_EQ(error_state->stopped, 0);
  EXPECT_TRUE(error_state->error == std::errc::connection_reset);
}

// Contract coverage for io_context::stop() aborting an inflight SSL read:
// the completion must be set_value(operation_canceled, ...) on the
// downstream receiver, not set_stopped. The peer never writes, so the SSL
// read parks on EVFILT_READ via the transport layer; context.stop() aborts
// the inflight I/O and the SSL read-all loop reports
// set_value(operation_canceled, transferred) to the transfer_receiver.
TEST(SslTransferTest, inflight_ssl_read_aborted_by_io_context_stop) {
  test_certificate_files files;

  bnio::ssl_context server_context(bnio::ssl_context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());

  bnio::ssl_context client_context(bnio::ssl_context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);

  bnio::ssl_stream client{bnio::tcp_socket(sockets[0]), client_context};
  bnio::ssl_stream server{bnio::tcp_socket(sockets[1]), server_context};

  // Handshake in a separate io_context (same pattern as the transfer test).
  {
    bnio::io_context context;
    if (!context.is_open()) {
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    auto scheduler = context.get_post_scheduler();

    auto state = std::make_shared<handshake_state>();
    handshake_receiver client_receiver{state, &context};
    handshake_receiver server_receiver{state, &context};

    auto client_sender =
        client.async_handshake(scheduler, bnio::ssl_handshake_type::client);
    auto server_sender =
        server.async_handshake(scheduler, bnio::ssl_handshake_type::server);

    auto client_operation =
        bexec::connect(std::move(client_sender), std::move(client_receiver));
    auto server_operation =
        bexec::connect(std::move(server_sender), std::move(server_receiver));

    bexec::start(client_operation);
    bexec::start(server_operation);
    context.run();

    EXPECT_EQ(state->values, 2);
    EXPECT_EQ(state->errors, 0);
    EXPECT_EQ(state->stopped, 0);
  }

  // New io_context: SSL read blocks because the peer never writes.
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  std::array<char, 16> bytes{};
  auto state = std::make_shared<transfer_state>();
  // context=nullptr, completions=nullptr: receiver does NOT self-stop;
  // the main thread calls context.stop() to interrupt the inflight read.
  transfer_receiver receiver{state, nullptr, nullptr, 0};

  auto sender = server.async_read(scheduler, bnio::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  // Schedule a task to confirm the worker is active before calling stop().
  std::atomic<bool> worker_active{false};
  struct active_receiver {
    std::atomic<bool>* flag;
    void set_value(std::error_code) noexcept {
      flag->store(true, std::memory_order_release);
    }
    void set_stopped() noexcept {
      flag->store(true, std::memory_order_release);
    }
  };
  auto schedule_sender = context.get_post_scheduler().schedule();
  auto schedule_op =
      bexec::connect(schedule_sender, active_receiver{&worker_active});
  bexec::start(schedule_op);

  std::thread worker([&context] { context.run(); });

  {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!worker_active.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        (void)context.stop();
        worker.join();
        FAIL() << "Timed out waiting for worker to become active";
      }
      std::this_thread::yield();
    }
    // Margin: let the SSL read park on EVFILT_READ before stop().
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  EXPECT_GE(context.stop(), 0);
  worker.join();

  // Contract: context stop aborting inflight I/O completes via
  // set_value(operation_canceled), not set_stopped.
  EXPECT_EQ(state->stopped, 0u);
  EXPECT_EQ(state->values, 0u);
  EXPECT_EQ(state->errors, 1u);
  EXPECT_TRUE(state->error ==
              std::make_error_code(std::errc::operation_canceled));
}

// Contract coverage for the stop-token early check in
// ssl_io_operation::start(): when the receiver's stop token is already
// requested before start(), the operation observes it and completes via
// set_stopped without entering the repeat_until loop.
TEST(SslTransferTest, pre_stopped_ssl_read_stops) {
  test_certificate_files files;

  bnio::ssl_context server_context(bnio::ssl_context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());

  bnio::ssl_context client_context(bnio::ssl_context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);

  bnio::ssl_stream client{bnio::tcp_socket(sockets[0]), client_context};
  bnio::ssl_stream server{bnio::tcp_socket(sockets[1]), server_context};

  // Handshake first.
  {
    bnio::io_context context;
    if (!context.is_open()) {
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    auto scheduler = context.get_post_scheduler();

    auto hs_state = std::make_shared<handshake_state>();
    handshake_receiver client_receiver{hs_state, &context};
    handshake_receiver server_receiver{hs_state, &context};

    auto client_sender =
        client.async_handshake(scheduler, bnio::ssl_handshake_type::client);
    auto server_sender =
        server.async_handshake(scheduler, bnio::ssl_handshake_type::server);

    auto client_operation =
        bexec::connect(std::move(client_sender), std::move(client_receiver));
    auto server_operation =
        bexec::connect(std::move(server_sender), std::move(server_receiver));

    bexec::start(client_operation);
    bexec::start(server_operation);
    context.run();

    EXPECT_EQ(hs_state->values, 2);
    EXPECT_EQ(hs_state->errors, 0);
    EXPECT_EQ(hs_state->stopped, 0);
  }

  // New io_context with stop already requested.
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  bexec::inplace_stop_source source;
  EXPECT_TRUE(source.request_stop());

  std::array<char, 16> bytes{};
  auto state = std::make_shared<transfer_state>();
  unsigned completions = 0;
  ssl_stopped_transfer_receiver receiver;
  receiver.state = state;
  receiver.context = &context;
  receiver.completions = &completions;
  receiver.target = 1;
  receiver.env = stop_env{source.get_token()};

  auto sender = server.async_read(scheduler, bnio::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  // Contract: a stop token already canceled at start() is observed by the
  // SSL read and completes via set_stopped (not set_value(operation_canceled)).
  EXPECT_EQ(state->errors, 0u);
  EXPECT_EQ(state->values, 0u);
  EXPECT_EQ(state->stopped, 1u);
}

// Covers ssl_io_operation::start() empty-buffer early return
// (operation.h:99-101): a zero-size buffer must complete synchronously with
// set_value(ec={}, 0) without entering the repeat_until loop.
TEST(SslTransferTest, empty_buffer_ssl_read_reports_success) {
  test_certificate_files files;

  bnio::ssl_context server_context(bnio::ssl_context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());

  bnio::ssl_context client_context(bnio::ssl_context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);

  bnio::ssl_stream client{bnio::tcp_socket(sockets[0]), client_context};
  bnio::ssl_stream server{bnio::tcp_socket(sockets[1]), server_context};

  // Handshake first.
  {
    bnio::io_context context;
    if (!context.is_open()) {
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    auto scheduler = context.get_post_scheduler();

    auto hs_state = std::make_shared<handshake_state>();
    handshake_receiver client_receiver{hs_state, &context};
    handshake_receiver server_receiver{hs_state, &context};

    auto client_sender =
        client.async_handshake(scheduler, bnio::ssl_handshake_type::client);
    auto server_sender =
        server.async_handshake(scheduler, bnio::ssl_handshake_type::server);

    auto client_operation =
        bexec::connect(std::move(client_sender), std::move(client_receiver));
    auto server_operation =
        bexec::connect(std::move(server_sender), std::move(server_receiver));

    bexec::start(client_operation);
    bexec::start(server_operation);
    context.run();

    EXPECT_EQ(hs_state->values, 2);
    EXPECT_EQ(hs_state->errors, 0);
    EXPECT_EQ(hs_state->stopped, 0);
  }

  // New io_context: SSL read with a zero-size buffer.
  bnio::io_context context;
  if (!context.is_open()) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  std::array<char, 16> bytes{};
  auto state = std::make_shared<transfer_state>();
  unsigned completions = 0;
  transfer_receiver receiver{state, &context, &completions, 1};

  auto sender = server.async_read(scheduler, bnio::buffer(bytes.data(), 0));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->values, 1u);
  EXPECT_EQ(state->bytes, 0u);
  EXPECT_EQ(state->errors, 0u);
  EXPECT_EQ(state->stopped, 0u);
}

}  // namespace
