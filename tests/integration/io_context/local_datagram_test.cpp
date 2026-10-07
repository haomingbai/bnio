// Datagram semantics for the local (AF_UNIX) family: connected
// async_send/async_receive, unconnected async_send_to/async_receive_from
// with endpoint capture, and the unnamed-peer decode to the unspecified
// endpoint kind (docs/design/architecture/local-socket-split.md §3.4,
// §7.3).

#include <bnio/local.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <string_view>
#include <system_error>

#include "../../support/io_context/io_context_runtime_test_support.h"

namespace {

using bnio::async_io::local::endpoint;
using bnio::async_io::local::endpoint_kind;
using bnio::local::datagram_socket;

// Receiver for async_receive_from: records the decoded source endpoint.
struct datagram_from_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bnio::io_context* context = nullptr;
  endpoint* source = nullptr;

  void set_value(std::error_code ec, std::size_t size) noexcept {
    if (ec) {
      state->signal = signal_kind::error;
      state->error = ec;
    } else {
      state->signal = signal_kind::value;
      state->size = size;
    }
    (void)context->stop();
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    (void)context->stop();
  }
};

TEST(LocalDatagramTest, connected_send_receive_preserves_datagram) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  const char* socket_path = "/tmp/bnio-dgram-connected.sock";
  (void)::unlink(socket_path);

  datagram_socket receiver_socket;
  ASSERT_FALSE(
      receiver_socket.open(bnio::async_io::local::datagram_protocol{}));
  ASSERT_FALSE(receiver_socket.bind(endpoint(socket_path)));

  datagram_socket sender_socket;
  ASSERT_FALSE(sender_socket.open(bnio::async_io::local::datagram_protocol{}));
  ASSERT_FALSE(sender_socket.connect(endpoint(socket_path)));

  constexpr std::string_view payload = "one local datagram";
  byte_receiver read_receiver;
  read_receiver.context = &context;
  auto read_state = read_receiver.state;
  byte_receiver write_receiver;
  write_receiver.context = &context;
  auto write_state = write_receiver.state;

  std::array<char, 64> buffer{};
  auto write_operation =
      bexec::connect(sender_socket.async_send(scheduler, bnio::buffer(payload)),
                     std::move(write_receiver));
  auto read_operation = bexec::connect(
      receiver_socket.async_receive(scheduler, bnio::buffer(buffer)),
      std::move(read_receiver));
  bexec::start(write_operation);
  bexec::start(read_operation);
  context.run();

  ASSERT_EQ(write_state->signal, signal_kind::value);
  ASSERT_EQ(read_state->signal, signal_kind::value);
  EXPECT_EQ(read_state->size, payload.size());
  EXPECT_TRUE(std::memcmp(buffer.data(), payload.data(), payload.size()) == 0);

  (void)::unlink(socket_path);
}

TEST(LocalDatagramTest, send_to_receive_from_captures_bound_source_path) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  const char* receiver_path = "/tmp/bnio-dgram-rx.sock";
  const char* sender_path = "/tmp/bnio-dgram-tx.sock";
  (void)::unlink(receiver_path);
  (void)::unlink(sender_path);

  datagram_socket receiver_socket;
  ASSERT_FALSE(
      receiver_socket.open(bnio::async_io::local::datagram_protocol{}));
  ASSERT_FALSE(receiver_socket.bind(endpoint(receiver_path)));

  // The sender is bound, so the receiver can decode its source path.
  datagram_socket sender_socket;
  ASSERT_FALSE(sender_socket.open(bnio::async_io::local::datagram_protocol{}));
  ASSERT_FALSE(sender_socket.bind(endpoint(sender_path)));

  byte_receiver write_receiver;
  write_receiver.context = &context;
  auto write_state = write_receiver.state;
  datagram_from_receiver read_receiver;
  read_receiver.context = &context;
  auto read_state = read_receiver.state;

  constexpr std::string_view payload = "addressed datagram";
  endpoint source;
  std::array<char, 64> buffer{};
  auto write_operation = bexec::connect(
      sender_socket.async_send_to(scheduler, bnio::buffer(payload),
                                  endpoint(receiver_path)),
      std::move(write_receiver));
  auto read_operation =
      bexec::connect(receiver_socket.async_receive_from(
                         scheduler, bnio::buffer(buffer), source),
                     std::move(read_receiver));
  bexec::start(write_operation);
  bexec::start(read_operation);
  context.run();

  ASSERT_EQ(write_state->signal, signal_kind::value);
  ASSERT_EQ(read_state->signal, signal_kind::value);
  EXPECT_EQ(read_state->size, payload.size());
  EXPECT_TRUE(std::memcmp(buffer.data(), payload.data(), payload.size()) == 0);

  // The captured source is the sender's bound path.
  EXPECT_EQ(source.kind(), endpoint_kind::path_name);
  EXPECT_EQ(source.path(), sender_path);

  (void)::unlink(receiver_path);
  (void)::unlink(sender_path);
}

TEST(LocalDatagramTest, unbound_sender_decodes_as_unnamed) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  const char* receiver_path = "/tmp/bnio-dgram-unnamed.sock";
  (void)::unlink(receiver_path);

  datagram_socket receiver_socket;
  ASSERT_FALSE(
      receiver_socket.open(bnio::async_io::local::datagram_protocol{}));
  ASSERT_FALSE(receiver_socket.bind(endpoint(receiver_path)));

  // The sender never binds: the kernel reports an unnamed source, which
  // decodes to the unspecified endpoint kind.
  datagram_socket sender_socket;
  ASSERT_FALSE(sender_socket.open(bnio::async_io::local::datagram_protocol{}));

  byte_receiver write_receiver;
  write_receiver.context = &context;
  auto write_state = write_receiver.state;
  datagram_from_receiver read_receiver;
  read_receiver.context = &context;
  auto read_state = read_receiver.state;

  constexpr std::string_view payload = "unnamed datagram";
  endpoint source;
  std::array<char, 64> buffer{};
  auto write_operation = bexec::connect(
      sender_socket.async_send_to(scheduler, bnio::buffer(payload),
                                  endpoint(receiver_path)),
      std::move(write_receiver));
  auto read_operation =
      bexec::connect(receiver_socket.async_receive_from(
                         scheduler, bnio::buffer(buffer), source),
                     std::move(read_receiver));
  bexec::start(write_operation);
  bexec::start(read_operation);
  context.run();

  ASSERT_EQ(write_state->signal, signal_kind::value);
  ASSERT_EQ(read_state->signal, signal_kind::value);
  EXPECT_EQ(read_state->size, payload.size());
  EXPECT_EQ(source.kind(), endpoint_kind::unspecified);
  EXPECT_TRUE(source.path().empty());

  (void)::unlink(receiver_path);
}

TEST(LocalDatagramTest, owner_endpoint_queries_report_bound_names) {
  datagram_socket receiver_socket;
  ASSERT_FALSE(
      receiver_socket.open(bnio::async_io::local::datagram_protocol{}));

  const char* socket_path = "/tmp/bnio-dgram-name.sock";
  (void)::unlink(socket_path);
  ASSERT_FALSE(receiver_socket.bind(endpoint(socket_path)));

  endpoint local;
  ASSERT_FALSE(receiver_socket.local_endpoint(local));
  EXPECT_EQ(local.kind(), endpoint_kind::path_name);
  EXPECT_EQ(local.path(), socket_path);

  // An unbound datagram socket reports the unspecified kind (unnamed).
  datagram_socket unbound;
  ASSERT_FALSE(unbound.open(bnio::async_io::local::datagram_protocol{}));
  endpoint unbound_name;
  ASSERT_FALSE(unbound.local_endpoint(unbound_name));
  EXPECT_EQ(unbound_name.kind(), endpoint_kind::unspecified);

  (void)::unlink(socket_path);
}

}  // namespace
