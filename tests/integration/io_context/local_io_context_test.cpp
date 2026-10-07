// io_context integration for the local (AF_UNIX) family: the eager
// immediate-I/O toggle (on/off) for local reads and writes, defer-kind
// local I/O, and async_accept delivering a bnio::local::stream_socket
// (docs/design/architecture/local-socket-split.md §3.5, §3.6, §7.3).
//
// The observation points mirror io_context_eager_optional_test.cpp and
// io_context_defer_scheduler_test.cpp: with the eager probe enabled the
// payload crosses the socketpair during start(), before run().

#include <bnio/local.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <string_view>

#include "../../support/io_context/io_context_runtime_test_support.h"

#if !defined(SOCK_CLOEXEC)
#define SOCK_CLOEXEC 0
#endif
#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

namespace {

using stream_socket = bnio::local::stream_socket;

[[nodiscard]] std::array<int, 2> make_socketpair() {
  int sockets[2] = {-1, -1};
  const int rc = ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets);
  EXPECT_EQ(rc, 0);
  return {sockets[0], sockets[1]};
}

[[nodiscard]] bnio::io_context_options eager_options(bool eager) {
  bnio::io_context_options options;
  options.enable_immediate_io = eager;
  return options;
}

template <bool Eager>
void local_write_eager_toggle() {
  bnio::io_context context(eager_options(Eager));
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  auto sockets = make_socketpair();
  stream_socket sender_socket(sockets[0]);
  stream_socket receiver_socket(sockets[1]);

  constexpr std::string_view payload = "local-eager";
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto operation =
      bexec::connect(scheduler.async_write(sender_socket.view(),
                                           bnio::buffer(payload), MSG_NOSIGNAL),
                     std::move(receiver));
  bexec::start(operation);

  std::array<char, 32> peek{};
  const ssize_t peeked = ::recv(receiver_socket.native_handle(), peek.data(),
                                peek.size(), MSG_PEEK | MSG_DONTWAIT);
  if constexpr (Eager) {
    // The eager probe crossed the pair during start().
    EXPECT_EQ(peeked, static_cast<ssize_t>(payload.size()));
  } else {
    // Without the eager switch the write waits for io_uring submission.
    EXPECT_EQ(peeked, -1);
    EXPECT_EQ(errno, EAGAIN);
  }

  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());
}

template <bool Eager>
void local_read_eager_toggle() {
  bnio::io_context context(eager_options(Eager));
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  auto sockets = make_socketpair();
  stream_socket receiver_socket(sockets[0]);
  stream_socket sender_socket(sockets[1]);

  constexpr std::string_view payload = "local-eager-read";
  EXPECT_EQ(::send(sender_socket.native_handle(), payload.data(),
                   payload.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(payload.size()));

  std::array<char, 32> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto operation = bexec::connect(
      scheduler.async_read_some(receiver_socket.view(), bnio::buffer(bytes)),
      std::move(receiver));
  bexec::start(operation);

  if constexpr (Eager) {
    // The eager probe consumed the pending bytes during start(): the peer
    // queue is empty before run() even begins.
    std::array<char, 32> peek{};
    EXPECT_EQ(::recv(receiver_socket.native_handle(), peek.data(), peek.size(),
                     MSG_PEEK | MSG_DONTWAIT),
              -1);
    EXPECT_EQ(errno, EAGAIN);
  }

  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

TEST(LocalIoContextTest, local_write_completes_eagerly_with_eager_on) {
  local_write_eager_toggle<true>();
}

TEST(LocalIoContextTest, local_write_waits_for_submission_with_eager_off) {
  local_write_eager_toggle<false>();
}

TEST(LocalIoContextTest, local_read_completes_eagerly_with_eager_on) {
  local_read_eager_toggle<true>();
}

TEST(LocalIoContextTest, local_read_completes_passively_with_eager_off) {
  local_read_eager_toggle<false>();
}

TEST(LocalIoContextTest, defer_kind_local_write_keeps_eager_probe) {
  bnio::io_context_options options;
  options.enable_immediate_io = true;
  bnio::io_context context(options);
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  auto sockets = make_socketpair();
  stream_socket sender_socket(sockets[0]);
  stream_socket receiver_socket(sockets[1]);

  constexpr std::string_view payload = "defer-local";
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto operation = bexec::connect(
      context.get_defer_scheduler().async_write(
          sender_socket.view(), bnio::buffer(payload), MSG_NOSIGNAL),
      std::move(receiver));
  bexec::start(operation);

  // The eager probe pushed the bytes into the peer's receive queue during
  // start(), on the starting thread, for every kind that keeps k_immediate
  // enabled — the local family included.
  std::array<char, 32> peek{};
  EXPECT_EQ(::recv(receiver_socket.native_handle(), peek.data(), peek.size(),
                   MSG_PEEK | MSG_DONTWAIT),
            static_cast<ssize_t>(payload.size()));

  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());
}

TEST(LocalIoContextTest, defer_kind_local_read_keeps_eager_probe) {
  bnio::io_context_options options;
  options.enable_immediate_io = true;
  bnio::io_context context(options);
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  auto sockets = make_socketpair();
  stream_socket receiver_socket(sockets[0]);
  stream_socket sender_socket(sockets[1]);

  constexpr std::string_view payload = "defer-local-read";
  EXPECT_EQ(::send(sender_socket.native_handle(), payload.data(),
                   payload.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(payload.size()));

  std::array<char, 32> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto operation =
      bexec::connect(context.get_defer_scheduler().async_read_some(
                         receiver_socket.view(), bnio::buffer(bytes)),
                     std::move(receiver));
  bexec::start(operation);

  // The eager probe consumed the data during start(), before run().
  std::array<char, 32> peek{};
  EXPECT_EQ(::recv(receiver_socket.native_handle(), peek.data(), peek.size(),
                   MSG_PEEK | MSG_DONTWAIT),
            -1);
  EXPECT_EQ(errno, EAGAIN);

  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

TEST(LocalIoContextTest, async_accept_delivers_local_stream_socket) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  const char* socket_path = "/tmp/bnio-accept-owner.sock";
  (void)::unlink(socket_path);

  bnio::local::stream_acceptor acceptor;
  ASSERT_FALSE(acceptor.open(bnio::async_io::local::stream_protocol{}));
  ASSERT_FALSE(acceptor.bind(bnio::async_io::local::endpoint(socket_path)));
  ASSERT_FALSE(acceptor.listen(4));

  stream_socket client;
  ASSERT_FALSE(client.open(bnio::async_io::local::stream_protocol{}));

  // The accept receiver keeps the delivered owner alive.
  struct owner_accept_receiver {
    std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
    bnio::io_context* context = nullptr;
    stream_socket* storage = nullptr;

    void set_value(std::error_code ec, stream_socket socket) noexcept {
      if (ec) {
        state->signal = signal_kind::error;
        state->error = ec;
      } else {
        state->signal = signal_kind::value;
        *storage = std::move(socket);
      }
      (void)context->stop();
    }

    void set_stopped() noexcept {
      state->signal = signal_kind::stopped;
      (void)context->stop();
    }
  };

  stream_socket accepted;
  owner_accept_receiver accept_receiver;
  accept_receiver.context = &context;
  accept_receiver.storage = &accepted;
  auto accept_state = accept_receiver.state;

  void_receiver connect_receiver;
  connect_receiver.context = &context;
  auto connect_state = connect_receiver.state;

  auto accept_operation = bexec::connect(acceptor.async_accept(scheduler),
                                         std::move(accept_receiver));
  auto connect_operation = bexec::connect(
      client.async_connect(scheduler,
                           bnio::async_io::local::endpoint(socket_path)),
      std::move(connect_receiver));
  bexec::start(accept_operation);
  bexec::start(connect_operation);
  context.run();

  ASSERT_EQ(accept_state->signal, signal_kind::value);
  EXPECT_EQ(connect_state->signal, signal_kind::value);

  // The delivered owner is usable directly: a raw send through its
  // descriptor reaches the client.
  constexpr std::string_view payload = "accepted-owner";
  EXPECT_EQ(::send(accepted.native_handle(), payload.data(), payload.size(),
                   MSG_NOSIGNAL),
            static_cast<ssize_t>(payload.size()));
  std::array<char, 32> bytes{};
  EXPECT_EQ(::recv(client.native_handle(), bytes.data(), bytes.size(), 0),
            static_cast<ssize_t>(payload.size()));
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);

  (void)::unlink(socket_path);
}

}  // namespace
