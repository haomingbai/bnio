// Stream lifecycle for the local (AF_UNIX) family through the owner types:
// open -> bind -> listen -> async_accept -> async_connect over a filesystem
// path and over an abstract address, full-duplex echo, and shutdown/close
// semantics (docs/design/architecture/local-socket-split.md §7.3).

#include <bnio/io_context.h>
#include <bnio/local.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <string>
#include <system_error>

#include "../../support/io_context/io_context_runtime_test_support.h"

#if defined(BNIO_HAS_ASYNC_IO_LINUX)
#include <sys/un.h>
#endif

namespace {

using bnio::async_io::local::endpoint;
using bnio::async_io::local::endpoint_kind;

// Receiver for async_accept on a local::stream_acceptor: stores the
// delivered owner instead of releasing its descriptor.
struct local_accept_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bnio::io_context* context = nullptr;
  bnio::local::stream_socket* storage = nullptr;

  void set_value(std::error_code ec,
                 bnio::local::stream_socket socket) noexcept {
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

void unlink_quietly(const char* path) { (void)::unlink(path); }

// Phase runner: every phase connects its senders on the caller's stack and
// stops the context when all targeted completions have landed.
void run_phase(bnio::io_context& context) { context.run(); }

TEST(LocalStreamLifecycleTest, open_bind_listen_accept_connect_over_path) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  const char* socket_path = "/tmp/bnio-lifecycle-path.sock";
  unlink_quietly(socket_path);

  bnio::local::stream_acceptor acceptor;
  ASSERT_FALSE(acceptor.open(bnio::async_io::local::stream_protocol{}));
  ASSERT_FALSE(acceptor.bind(endpoint(socket_path)));
  ASSERT_FALSE(acceptor.listen(4));

  bnio::local::stream_socket client;
  ASSERT_FALSE(client.open(bnio::async_io::local::stream_protocol{}));

  bnio::local::stream_socket accepted;

  local_accept_receiver accept_receiver;
  accept_receiver.context = &context;
  accept_receiver.storage = &accepted;
  auto accept_state = accept_receiver.state;

  void_receiver connect_receiver;
  connect_receiver.context = &context;
  auto connect_state = connect_receiver.state;

  auto accept_operation =
      bexec::connect(acceptor.async_accept(scheduler, SOCK_CLOEXEC),
                     std::move(accept_receiver));
  auto connect_operation =
      bexec::connect(client.async_connect(scheduler, endpoint(socket_path)),
                     std::move(connect_receiver));

  bexec::start(accept_operation);
  bexec::start(connect_operation);
  run_phase(context);

  ASSERT_EQ(accept_state->signal, signal_kind::value);
  EXPECT_EQ(connect_state->signal, signal_kind::value);

  // The accepted descriptor is a live socket, nonblocking like every owner.
  ASSERT_TRUE(accepted.is_open());
  EXPECT_TRUE((::fcntl(accepted.native_handle(), F_GETFL, 0) & O_NONBLOCK) !=
              0);

  // The client's peer is the acceptor's bound path: the sockaddr_un
  // roundtrip through a real connection decodes as a path-name endpoint.
  endpoint peer;
  ASSERT_FALSE(client.remote_endpoint(peer));
  EXPECT_EQ(peer.kind(), endpoint_kind::path_name);
  EXPECT_EQ(peer.path(), socket_path);

  // The bound socket file exists while the acceptor is open.
  EXPECT_EQ(::access(socket_path, F_OK), 0);

  (void)acceptor.close();
  unlink_quietly(socket_path);
}

#if defined(BNIO_HAS_ASYNC_IO_LINUX)
TEST(LocalStreamLifecycleTest, open_bind_listen_accept_connect_over_abstract) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  const char* abstract_name = "bnio-lifecycle-abstract";

  bnio::local::stream_acceptor acceptor;
  ASSERT_FALSE(acceptor.open(bnio::async_io::local::stream_protocol{}));
  ASSERT_FALSE(acceptor.bind(endpoint::abstract(abstract_name)));
  ASSERT_FALSE(acceptor.listen(4));

  bnio::local::stream_socket client;
  ASSERT_FALSE(client.open(bnio::async_io::local::stream_protocol{}));

  bnio::local::stream_socket accepted;

  local_accept_receiver accept_receiver;
  accept_receiver.context = &context;
  accept_receiver.storage = &accepted;
  auto accept_state = accept_receiver.state;

  void_receiver connect_receiver;
  connect_receiver.context = &context;
  auto connect_state = connect_receiver.state;

  auto accept_operation = bexec::connect(acceptor.async_accept(scheduler),
                                         std::move(accept_receiver));
  auto connect_operation = bexec::connect(
      client.async_connect(scheduler, endpoint::abstract(abstract_name)),
      std::move(connect_receiver));

  bexec::start(accept_operation);
  bexec::start(connect_operation);
  run_phase(context);

  EXPECT_EQ(accept_state->signal, signal_kind::value);
  EXPECT_EQ(connect_state->signal, signal_kind::value);

  endpoint peer;
  ASSERT_FALSE(client.remote_endpoint(peer));
  EXPECT_EQ(peer.kind(), endpoint_kind::abstract);
  EXPECT_EQ(peer.path(), abstract_name);
}
#endif

TEST(LocalStreamLifecycleTest, full_duplex_echo) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  const char* socket_path = "/tmp/bnio-lifecycle-echo.sock";
  unlink_quietly(socket_path);

  bnio::local::stream_acceptor acceptor;
  ASSERT_FALSE(acceptor.open(bnio::async_io::local::stream_protocol{}));
  ASSERT_FALSE(acceptor.bind(endpoint(socket_path)));
  ASSERT_FALSE(acceptor.listen(4));

  bnio::local::stream_socket client;
  ASSERT_FALSE(client.open(bnio::async_io::local::stream_protocol{}));

  bnio::local::stream_socket accepted;

  // Phase 1: establish the connection.
  local_accept_receiver accept_receiver;
  accept_receiver.context = &context;
  accept_receiver.storage = &accepted;
  auto accept_state = accept_receiver.state;
  void_receiver connect_receiver;
  connect_receiver.context = &context;
  auto connect_state = connect_receiver.state;

  auto accept_operation = bexec::connect(acceptor.async_accept(scheduler),
                                         std::move(accept_receiver));
  auto connect_operation =
      bexec::connect(client.async_connect(scheduler, endpoint(socket_path)),
                     std::move(connect_receiver));
  bexec::start(accept_operation);
  bexec::start(connect_operation);
  run_phase(context);

  ASSERT_EQ(accept_state->signal, signal_kind::value);
  ASSERT_EQ(connect_state->signal, signal_kind::value);

  // Phase 2: client -> server. Both ops live on this stack frame across
  // the run loop.
  constexpr std::string_view payload = "duplex echo payload";
  byte_receiver write_receiver;
  write_receiver.context = &context;
  auto write_state = write_receiver.state;
  byte_receiver read_receiver;
  read_receiver.context = &context;
  auto read_state = read_receiver.state;

  std::array<char, 64> server_buffer{};
  auto client_write_operation = bexec::connect(
      client.async_write(scheduler, bnio::buffer(payload), MSG_NOSIGNAL),
      std::move(write_receiver));
  auto server_read_operation = bexec::connect(
      accepted.async_read_some(scheduler, bnio::buffer(server_buffer)),
      std::move(read_receiver));
  bexec::start(client_write_operation);
  bexec::start(server_read_operation);
  run_phase(context);

  ASSERT_EQ(write_state->signal, signal_kind::value);
  ASSERT_EQ(read_state->signal, signal_kind::value);
  EXPECT_EQ(write_state->size, payload.size());
  EXPECT_EQ(read_state->size, payload.size());
  EXPECT_TRUE(
      std::memcmp(server_buffer.data(), payload.data(), payload.size()) == 0);

  // Phase 3: server -> client.
  byte_receiver write_back_receiver;
  write_back_receiver.context = &context;
  auto write_back_state = write_back_receiver.state;
  byte_receiver read_back_receiver;
  read_back_receiver.context = &context;
  auto read_back_state = read_back_receiver.state;

  std::array<char, 64> client_buffer{};
  auto server_write_operation = bexec::connect(
      accepted.async_write(scheduler,
                           bnio::buffer(server_buffer.data(), read_state->size),
                           MSG_NOSIGNAL),
      std::move(write_back_receiver));
  auto client_read_operation = bexec::connect(
      client.async_read_some(scheduler, bnio::buffer(client_buffer)),
      std::move(read_back_receiver));
  bexec::start(server_write_operation);
  bexec::start(client_read_operation);
  run_phase(context);

  ASSERT_EQ(write_back_state->signal, signal_kind::value);
  ASSERT_EQ(read_back_state->signal, signal_kind::value);
  EXPECT_EQ(read_back_state->size, payload.size());
  EXPECT_TRUE(
      std::memcmp(client_buffer.data(), payload.data(), payload.size()) == 0);

  unlink_quietly(socket_path);
}

TEST(LocalStreamLifecycleTest, shutdown_and_close_semantics) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  const char* socket_path = "/tmp/bnio-lifecycle-shutdown.sock";
  unlink_quietly(socket_path);

  bnio::local::stream_acceptor acceptor;
  ASSERT_FALSE(acceptor.open(bnio::async_io::local::stream_protocol{}));
  ASSERT_FALSE(acceptor.bind(endpoint(socket_path)));
  ASSERT_FALSE(acceptor.listen(4));

  bnio::local::stream_socket client;
  ASSERT_FALSE(client.open(bnio::async_io::local::stream_protocol{}));
  bnio::local::stream_socket accepted;

  local_accept_receiver accept_receiver;
  accept_receiver.context = &context;
  accept_receiver.storage = &accepted;
  auto accept_state = accept_receiver.state;
  void_receiver connect_receiver;
  connect_receiver.context = &context;
  auto connect_state = connect_receiver.state;

  auto accept_operation = bexec::connect(acceptor.async_accept(scheduler),
                                         std::move(accept_receiver));
  auto connect_operation =
      bexec::connect(client.async_connect(scheduler, endpoint(socket_path)),
                     std::move(connect_receiver));
  bexec::start(accept_operation);
  bexec::start(connect_operation);
  run_phase(context);

  ASSERT_EQ(accept_state->signal, signal_kind::value);
  ASSERT_EQ(connect_state->signal, signal_kind::value);

  // Shutdown the write side: the peer observes a zero-byte read (EOF).
  EXPECT_FALSE(client.shutdown(SHUT_WR));

  byte_receiver eof_receiver;
  eof_receiver.context = &context;
  auto eof_state = eof_receiver.state;
  std::array<char, 16> eof_buffer{};
  auto eof_operation =
      bexec::connect(accepted.async_read(scheduler, bnio::buffer(eof_buffer)),
                     std::move(eof_receiver));
  bexec::start(eof_operation);
  run_phase(context);

  ASSERT_EQ(eof_state->signal, signal_kind::value);
  EXPECT_EQ(eof_state->size, 0U);

  // close() releases the descriptor; a second close reports nothing.
  EXPECT_FALSE(client.close());
  EXPECT_FALSE(client.is_open());
  EXPECT_FALSE(client.close());

  // The destructor also closes: the accepted socket closes at scope exit,
  // so only the fd bookkeeping is pinned here.
  const int accepted_fd = accepted.native_handle();
  ASSERT_GE(accepted_fd, 0);
  EXPECT_FALSE(accepted.close());

  unlink_quietly(socket_path);
}

}  // namespace
