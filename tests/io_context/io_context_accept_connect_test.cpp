#include "io_context_loopback_test_support.h"

namespace {

void test_accept_connect_loopback() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  bupp::tcp_acceptor acceptor;
  assert(!acceptor.open(bupp::ip::tcp::v4()));
  assert(!acceptor.set_reuse_address(true));
  assert(!acceptor.bind(bupp::ip::endpoint::loopback_v4(0)));
  assert(!acceptor.listen(4));
  const bupp::ip::endpoint endpoint = bound_loopback_endpoint(acceptor);

  bupp::tcp_socket client;
  assert(!client.open(bupp::ip::tcp::v4()));

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

  assert(completions == 2);
  assert(accept_state->signal == signal_kind::value);
  assert(accept_state->fd >= 0);
  assert(connect_state->signal == signal_kind::value);
  assert(client.is_open());

  assert(::close(accept_state->fd) == 0);
  accept_state->fd = -1;
}

}  // namespace

int main() {
  test_accept_connect_loopback();
  return 0;
}
