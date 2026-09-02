/**
 * @file io_uring_eagain_rearm_test.cpp
 * @brief Regression tests for transient -EAGAIN routing on nonblocking
 * sockets.
 *
 * Contract (aligned with the BSD kqueue backend, kqueue_context::
 * perform_io_step()): a transient -EAGAIN on a nonblocking I/O attempt must
 * never reach the receiver as a terminal error. The kqueue backend re-arms
 * the operation and keeps it inflight; the io_uring backend must behave the
 * same instead of delivering set_value(EAGAIN) as a completion (error-code
 * consistency review, finding 1).
 *
 * The kernel delivers a -EAGAIN CQE only for requests marked REQ_F_NOWAIT
 * (RWF_NOWAIT, or an O_NONBLOCK file without nowait support); socket
 * operations are converted into an armed poll internally (v6.6
 * io_uring/io_uring.c io_queue_async), so these socket-based tests pin the
 * observable contract: data arriving after the submission eventually
 * completes the operation with success, and no EAGAIN is ever surfaced to
 * the receiver. The io_uring backend re-submits a would-block CQE through
 * the I/O queue (kqueue perform_io_step() parity), keeping that guarantee
 * also for the descriptor paths where a -EAGAIN CQE can be delivered.
 */

#include <gtest/gtest.h>

#include <bnio/async_io/buffer_view.h>
#include <bnio/async_io/ip/address.h>
#include <bnio/async_io/ip/endpoint.h>
#include <bnio/async_io/linux/io_uring_context_base/context.h>
#include <bnio/async_io/linux/io_uring_operations/socket.h>
#include <bnio/async_io/socket_view.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>

#include "../../support/async_io/io_uring_context_test_support.h"

namespace {

using namespace bnio_async_io_io_uring_test;

/** RAII nonblocking connected socket pair. */
class nonblocking_socket_pair {
 public:
  nonblocking_socket_pair() {
    ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
                 fds_.data());
  }

  ~nonblocking_socket_pair() {
    for (int fd : fds_) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  }

  nonblocking_socket_pair(const nonblocking_socket_pair&) = delete;
  nonblocking_socket_pair& operator=(const nonblocking_socket_pair&) = delete;

  [[nodiscard]] int operator[](std::size_t index) const {
    return fds_[index];
  }

  [[nodiscard]] bool valid() const { return fds_[0] >= 0 && fds_[1] >= 0; }

 private:
  std::array<int, 2> fds_{-1, -1};
};

TEST(IoUringEagainRearmTest,
     recv_on_empty_socket_completes_after_late_data) {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  nonblocking_socket_pair pair;
  ASSERT_TRUE(pair.valid());

  auto state = std::make_shared<shared_state>();
  std::array<char, 16> bytes{};
  io_uring_recv_operation<receiver> operation(
      context, stream_socket_view(pair[0]),
      buffer_view(bytes.data(), bytes.size()), 0,
      receiver{state, &context, /*stop_on_completion=*/true});

  std::thread worker([&context] { context.run(); });
  bexec::start(operation);

  // The recv is submitted while the socket is empty: any would-block
  // attempt must translate into a wait, never into a terminal EAGAIN.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ASSERT_EQ(::write(pair[1], "hello", 5), 5);

  worker.join();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->result, 5);
  EXPECT_EQ(std::memcmp(bytes.data(), "hello", 5), 0);
  EXPECT_TRUE(state->in_context);
}

TEST(IoUringEagainRearmTest,
     send_on_full_buffer_completes_after_reader_drains) {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  nonblocking_socket_pair pair;
  ASSERT_TRUE(pair.valid());

  // Synchronously fill the send buffer so the async send below must hit a
  // would-block attempt before the peer drains anything.
  std::array<char, 4096> filler{};
  ::memset(filler.data(), 0x5A, filler.size());
  for (;;) {
    const ssize_t written = ::write(pair[0], filler.data(), filler.size());
    if (written < 0) {
      ASSERT_EQ(errno, EAGAIN);
      break;
    }
    ASSERT_GT(written, 0);
  }

  constexpr std::size_t k_send_size = 8192;
  auto state = std::make_shared<shared_state>();
  std::array<char, k_send_size> payload{};
  ::memset(payload.data(), 0xA5, payload.size());
  io_uring_send_operation<receiver> operation(
      context, stream_socket_view(pair[0]),
      buffer_view(payload.data(), payload.size()), 0,
      receiver{state, &context, /*stop_on_completion=*/true});

  std::thread worker([&context] { context.run(); });
  bexec::start(operation);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Drain the peer so the pending send finds buffer space and completes.
  std::size_t drained = 0;
  std::array<char, 4096> drain{};
  for (;;) {
    const ssize_t n = ::read(pair[1], drain.data(), drain.size());
    if (n < 0) {
      ASSERT_EQ(errno, EAGAIN);
      break;
    }
    if (n == 0) {
      break;
    }
    drained += static_cast<std::size_t>(n);
  }
  ASSERT_GT(drained, 0U);

  worker.join();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->result, static_cast<int>(k_send_size));
  EXPECT_TRUE(state->in_context);
}

TEST(IoUringEagainRearmTest,
     accept_on_idle_listener_completes_after_late_connect) {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    GTEST_SKIP() << "io_uring is unavailable";
  }

  const int listener_fd =
      ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  ASSERT_GE(listener_fd, 0);

  sockaddr_in listener_address{};
  listener_address.sin_family = AF_INET;
  listener_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  listener_address.sin_port = 0;
  ASSERT_EQ(::bind(listener_fd, reinterpret_cast<const sockaddr*>(
                                       &listener_address),
                   sizeof(listener_address)), 0);
  ASSERT_EQ(::listen(listener_fd, 16), 0);
  sockaddr_in bound_address{};
  socklen_t bound_size = sizeof(bound_address);
  ASSERT_EQ(::getsockname(listener_fd, reinterpret_cast<sockaddr*>(
                                             &bound_address),
                          &bound_size), 0);

  auto state = std::make_shared<shared_state>();
  bnio::async_io::ip::endpoint remote;
  io_uring_accept_operation<receiver> operation(
      context, stream_socket_view(listener_fd), remote, 0,
      receiver{state, &context, /*stop_on_completion=*/true});

  std::thread worker([&context] { context.run(); });
  bexec::start(operation);

  // The accept is submitted while no connection is pending.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const int connector_fd =
      ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(connector_fd, 0);
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  target.sin_port = bound_address.sin_port;
  ASSERT_EQ(::connect(connector_fd, reinterpret_cast<const sockaddr*>(&target),
                      sizeof(target)), 0);

  worker.join();

  ::close(connector_fd);

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_GE(state->result, 0);
  EXPECT_TRUE(state->in_context);
}

}  // namespace
