// Coverage for the read-all semantics of io_context::async_read.
//
// async_read is read-all: it repeatedly reads until the buffer is full or
// EOF is observed. EOF (recv/pread returning 0) completes successfully with
// ec={} and the number of bytes read so far; a first-step error propagates
// its ec; a zero-length buffer completes synchronously with ec={} and size 0.
//
// Callers that need single-read behavior use async_read_some (see
// io_context_read_write_test.cpp).

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "../../support/io_context/io_context_runtime_test_support.h"

namespace {

TEST(IoContextReadAllTest, read_all_fills_buffer_exactly) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);

  // Payload exactly the size of the buffer: read-all fills the buffer and
  // completes with size == payload size.
  constexpr std::string_view payload = "0123456789abcdef";
  static_assert(payload.size() == 16);
  EXPECT_EQ(::send(sender_socket.native_handle(), payload.data(),
                   payload.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(payload.size()));

  std::array<char, 16> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = receiver_socket.async_read(scheduler, bnio::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

TEST(IoContextReadAllTest, read_all_completes_on_eof_with_partial_data) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);

  // Send fewer bytes than the buffer, then half-close: read-all observes the
  // partial chunk followed by EOF and completes successfully with the bytes
  // actually read.
  constexpr std::string_view payload = "hello";
  EXPECT_EQ(::send(sender_socket.native_handle(), payload.data(),
                   payload.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(payload.size()));
  EXPECT_EQ(::shutdown(sender_socket.native_handle(), SHUT_WR), 0);

  std::array<char, 16> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = receiver_socket.async_read(scheduler, bnio::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

TEST(IoContextReadAllTest, read_all_empty_eof_completes_success) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);

  // Peer closes immediately: the empty EOF is a successful zero-byte read.
  EXPECT_EQ(::shutdown(sender_socket.native_handle(), SHUT_WR), 0);

  std::array<char, 16> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = receiver_socket.async_read(scheduler, bnio::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, 0u);
}

TEST(IoContextReadAllTest, read_all_first_step_error_reports_ec) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  // An invalid descriptor makes the first read attempt fail with EBADF;
  // read-all propagates that ec instead of looping.
  std::array<char, 8> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = scheduler.async_read(bnio::async_io::descriptor_view(),
                                     bnio::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::error);
  EXPECT_EQ(state->error, std::error_code(EBADF, std::generic_category()));
}

TEST(IoContextReadAllTest,
     read_all_zero_length_buffer_completes_synchronously) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);

  std::array<char, 8> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender =
      receiver_socket.async_read(scheduler, bnio::buffer(bytes.data(), 0));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  // A zero-length buffer completes synchronously during start(): ec={}, size=0.
  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, 0u);

  context.run();
  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, 0u);
}

TEST(IoContextReadAllTest, read_all_loops_until_buffer_full) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);

  // Shrink the receiver buffer so the payload must be delivered in several
  // kernel chunks, forcing read-all to loop across multiple read attempts.
  constexpr int k_rcvbuf = 4096;
  EXPECT_EQ(::setsockopt(receiver_socket.native_handle(), SOL_SOCKET, SO_RCVBUF,
                         &k_rcvbuf, sizeof(k_rcvbuf)),
            0);

  constexpr std::size_t k_buffer_size = 16 * 1024;
  constexpr std::size_t k_first_chunk = 2048;
  static_assert(k_buffer_size > k_rcvbuf, "payload must exceed the buffer");
  static_assert(k_first_chunk < k_buffer_size);

  std::vector<char> payload(k_buffer_size);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>((i * 31 + 7) & 0xff);
  }

  std::array<char, k_buffer_size> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  // Pre-send a partial chunk so the eager first read observes fewer bytes
  // than the buffer, guaranteeing the read-all loop runs more than once.
  EXPECT_EQ(::send(sender_socket.native_handle(), payload.data(), k_first_chunk,
                   MSG_NOSIGNAL),
            static_cast<ssize_t>(k_first_chunk));

  auto sender = receiver_socket.async_read(scheduler, bnio::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  // Deliver the remainder from a separate thread while the context drains.
  // The payload exactly matches the buffer size, so read-all completes once
  // the buffer is full. The sender socket is non-blocking, so a full socket
  // buffer surfaces as EAGAIN: yield and retry instead of treating it as an
  // error, so the whole payload is eventually delivered.
  std::atomic<bool> send_error{false};
  std::thread sender_thread([&] {
    std::size_t sent = k_first_chunk;
    while (sent < payload.size()) {
      const ssize_t n =
          ::send(sender_socket.native_handle(), payload.data() + sent,
                 payload.size() - sent, MSG_NOSIGNAL);
      if (n > 0) {
        sent += static_cast<std::size_t>(n);
      } else if (n < 0 &&
                 (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        std::this_thread::yield();
      } else {
        send_error.store(true, std::memory_order_relaxed);
        break;
      }
    }
  });

  context.run();
  sender_thread.join();

  EXPECT_FALSE(send_error.load(std::memory_order_relaxed));
  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, k_buffer_size);
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), k_buffer_size) == 0);
}

}  // namespace
