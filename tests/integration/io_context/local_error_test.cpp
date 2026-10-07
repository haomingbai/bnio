// Error paths for the local (AF_UNIX) family: ECONNREFUSED on connect to a
// bound-but-not-listening path, EADDRINUSE on bind over an existing socket
// file, and the conversion-boundary std::errc::invalid_argument for
// endpoints that cannot become a sockaddr_un
// (docs/design/architecture/local-socket-split.md §6, §7.3).

#include <bnio/local.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <system_error>

#include "../../support/io_context/io_context_runtime_test_support.h"

namespace {

using bnio::async_io::local::endpoint;
using bnio::local::datagram_socket;
using bnio::local::stream_socket;

TEST(LocalErrorTest, connect_to_bound_but_not_listening_reports_econnrefused) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  const char* socket_path = "/tmp/bnio-err-refused.sock";
  (void)::unlink(socket_path);

  // Bound but never listening: connect fails with ECONNREFUSED.
  bnio::local::stream_acceptor acceptor;
  ASSERT_FALSE(acceptor.open(bnio::async_io::local::stream_protocol{}));
  ASSERT_FALSE(acceptor.bind(endpoint(socket_path)));

  stream_socket client;
  ASSERT_FALSE(client.open(bnio::async_io::local::stream_protocol{}));

  void_receiver connect_receiver;
  connect_receiver.context = &context;
  auto connect_state = connect_receiver.state;
  auto operation =
      bexec::connect(client.async_connect(scheduler, endpoint(socket_path)),
                     std::move(connect_receiver));
  bexec::start(operation);
  context.run();

  ASSERT_EQ(connect_state->signal, signal_kind::error);
  EXPECT_EQ(connect_state->error,
            std::error_code(ECONNREFUSED, std::generic_category()));

  (void)::unlink(socket_path);
}

// A truly nonexistent filesystem path fails connect(2) with ENOENT; the
// kernel's verdict governs. ECONNREFUSED is the pinned outcome for a bound
// (or stale) but non-listening path, asserted above.
TEST(LocalErrorTest, connect_to_missing_path_reports_enoent) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  const char* socket_path = "/tmp/bnio-err-missing-does-not-exist.sock";
  (void)::unlink(socket_path);

  stream_socket client;
  ASSERT_FALSE(client.open(bnio::async_io::local::stream_protocol{}));

  void_receiver connect_receiver;
  connect_receiver.context = &context;
  auto connect_state = connect_receiver.state;
  auto operation =
      bexec::connect(client.async_connect(scheduler, endpoint(socket_path)),
                     std::move(connect_receiver));
  bexec::start(operation);
  context.run();

  ASSERT_EQ(connect_state->signal, signal_kind::error);
  EXPECT_EQ(connect_state->error,
            std::error_code(ENOENT, std::generic_category()));
}

TEST(LocalErrorTest, bind_over_existing_socket_file_reports_eaddrinuse) {
  const char* socket_path = "/tmp/bnio-err-inuse.sock";
  (void)::unlink(socket_path);

  bnio::local::stream_acceptor first;
  ASSERT_FALSE(first.open(bnio::async_io::local::stream_protocol{}));
  ASSERT_FALSE(first.bind(endpoint(socket_path)));

  // The socket file exists; a second bind on it fails with EADDRINUSE and
  // the caller owns ::unlink for stale files.
  bnio::local::stream_acceptor second;
  ASSERT_FALSE(second.open(bnio::async_io::local::stream_protocol{}));
  const std::error_code error = second.bind(endpoint(socket_path));
  EXPECT_EQ(error, std::error_code(EADDRINUSE, std::generic_category()));

  (void)::unlink(socket_path);
}

TEST(LocalErrorTest, oversized_path_reports_invalid_argument) {
  // An oversized path cannot be represented; the endpoint degrades to the
  // unspecified kind and every consuming socket call reports
  // invalid_argument from the sockaddr_un conversion boundary, before any
  // syscall.
  const std::string oversized(
      bnio::async_io::local::endpoint::max_path_length + 1, 'a');
  const endpoint value(oversized);
  ASSERT_EQ(value.kind(), bnio::async_io::local::endpoint_kind::unspecified);

  bnio::local::stream_acceptor acceptor;
  ASSERT_FALSE(acceptor.open(bnio::async_io::local::stream_protocol{}));
  const std::error_code bind_error = acceptor.bind(value);
  EXPECT_EQ(bind_error, std::make_error_code(std::errc::invalid_argument));
  // No socket file was created for the rejected endpoint.
  EXPECT_EQ(::access(oversized.c_str(), F_OK), -1);

  stream_socket stream;
  ASSERT_FALSE(stream.open(bnio::async_io::local::stream_protocol{}));
  EXPECT_EQ(stream.view().connect(value),
            std::make_error_code(std::errc::invalid_argument));
  EXPECT_EQ(stream.view().bind(value),
            std::make_error_code(std::errc::invalid_argument));

  datagram_socket datagram;
  ASSERT_FALSE(datagram.open(bnio::async_io::local::datagram_protocol{}));
  EXPECT_EQ(datagram.bind(value),
            std::make_error_code(std::errc::invalid_argument));
  EXPECT_EQ(datagram.connect(value),
            std::make_error_code(std::errc::invalid_argument));
}

}  // namespace
