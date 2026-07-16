#include <gtest/gtest.h>

#include "io_context_loopback_test_support.h"

namespace {

TEST(IoContextAcceptConnectTest, accept_connect_loopback) {
  bupp::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  bupp::tcp_acceptor acceptor;
  EXPECT_TRUE(!acceptor.open(bupp::ip::tcp::v4()));
  EXPECT_TRUE(!acceptor.set_reuse_address(true));
  EXPECT_TRUE(!acceptor.bind(bupp::ip::endpoint::loopback_v4(0)));
  EXPECT_TRUE(!acceptor.listen(4));
  const bupp::ip::endpoint endpoint = bound_loopback_endpoint(acceptor);

  bupp::tcp_socket client;
  EXPECT_TRUE(!client.open(bupp::ip::tcp::v4()));

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

  EXPECT_TRUE(completions == 2);
  EXPECT_TRUE(accept_state->signal == signal_kind::value);
  EXPECT_TRUE(accept_state->fd >= 0);
#if defined(BUPP_HAS_IO_CONTEXT_BSD)
  EXPECT_TRUE((::fcntl(accept_state->fd, F_GETFL, 0) & O_NONBLOCK) != 0);
#endif
  EXPECT_TRUE(connect_state->signal == signal_kind::value);
  EXPECT_TRUE(client.is_open());

  EXPECT_TRUE(::close(accept_state->fd) == 0);
  accept_state->fd = -1;
}

}  // namespace
