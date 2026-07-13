#include "ssl_test_support.h"

namespace {

template <bool DirectSubmit>
void test_socketpair_handshake_is_io_context_driven() {
  bupp::io_context context;
  if (!context.is_open()) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  test_certificate_files files;

  bupp::ssl_context server_context(bupp::ssl_context_method::tls_server);
  test_require(server_context.valid());
  test_require(!server_context.use_certificate_chain_file(
      files.certificate.string().c_str()));
  test_require(
      !server_context.use_private_key_file(files.private_key.string().c_str()));
  test_require(!server_context.check_private_key());

  bupp::ssl_context client_context(bupp::ssl_context_method::tls_client);
  test_require(client_context.valid());
  client_context.set_verify_mode(SSL_VERIFY_NONE);

  int sockets[2] = {-1, -1};
  test_require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) ==
               0);

  bupp::ssl_stream client{bupp::tcp_socket(sockets[0]), client_context};
  bupp::ssl_stream server{bupp::tcp_socket(sockets[1]), server_context};

  auto state = std::make_shared<handshake_state>();
  handshake_receiver client_receiver{state, &context};
  handshake_receiver server_receiver{state, &context};

  auto client_sender = [&] {
    if constexpr (DirectSubmit) {
      return client.async_handshake_direct(scheduler,
                                           bupp::ssl_handshake_type::client);
    } else {
      return client.async_handshake(scheduler,
                                    bupp::ssl_handshake_type::client);
    }
  }();
  auto server_sender = [&] {
    if constexpr (DirectSubmit) {
      return server.async_handshake_direct(scheduler,
                                           bupp::ssl_handshake_type::server);
    } else {
      return server.async_handshake(scheduler,
                                    bupp::ssl_handshake_type::server);
    }
  }();

  auto client_operation =
      bexec::connect(std::move(client_sender), std::move(client_receiver));
  auto server_operation =
      bexec::connect(std::move(server_sender), std::move(server_receiver));

  bexec::start(client_operation);
  bexec::start(server_operation);
  if constexpr (DirectSubmit) {
    assert(scheduler.queued_io_size() == 0);
  }
  context.run();

  assert(state->values == 2);
  assert(state->errors == 0);
  assert(state->stopped == 0);
}

}  // namespace

int main() {
  test_socketpair_handshake_is_io_context_driven<false>();
  test_socketpair_handshake_is_io_context_driven<true>();
  return 0;
}
