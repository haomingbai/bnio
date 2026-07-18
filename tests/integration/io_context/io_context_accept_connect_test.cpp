#include <gtest/gtest.h>

#include "../../support/io_context/io_context_loopback_test_support.h"

namespace {

TEST(IoContextAcceptConnectTest, accept_connect_loopback) {
  bnio::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  bnio::tcp_acceptor acceptor;
  EXPECT_FALSE(acceptor.open(bnio::ip::tcp::v4()));
  EXPECT_FALSE(acceptor.set_reuse_address(true));
  EXPECT_FALSE(acceptor.bind(bnio::ip::endpoint::loopback_v4(0)));
  EXPECT_FALSE(acceptor.listen(4));
  const bnio::ip::endpoint endpoint = bound_loopback_endpoint(acceptor);

  bnio::tcp_socket client;
  EXPECT_FALSE(client.open(bnio::ip::tcp::v4()));

  unsigned completions = 0;

  socket_receiver accept_receiver;
  accept_receiver.context = &context;
  accept_receiver.completions = &completions;
  accept_receiver.target = 2;
  auto accept_state = accept_receiver.state;

  void_receiver connect_receiver;
  connect_receiver.context = &context;
  connect_receiver.completions = &completions;
  connect_receiver.target = 2;
  auto connect_state = connect_receiver.state;

  auto accept_sender = acceptor.async_accept(scheduler, SOCK_CLOEXEC);
  auto connect_sender = client.async_connect(scheduler, endpoint);

  auto accept_operation =
      bexec::connect(std::move(accept_sender), std::move(accept_receiver));
  auto connect_operation =
      bexec::connect(std::move(connect_sender), std::move(connect_receiver));

  bexec::start(accept_operation);
  bexec::start(connect_operation);
  context.run();

  EXPECT_EQ(completions, 2);
  EXPECT_EQ(accept_state->signal, signal_kind::value);
  EXPECT_TRUE(accept_state->fd >= 0);
#if defined(BNIO_HAS_IO_CONTEXT_BSD)
  EXPECT_TRUE((::fcntl(accept_state->fd, F_GETFL, 0) & O_NONBLOCK) != 0);
#endif
  EXPECT_EQ(connect_state->signal, signal_kind::value);
  EXPECT_TRUE(client.is_open());

  EXPECT_EQ(::close(accept_state->fd), 0);
  accept_state->fd = -1;
}

}  // namespace
