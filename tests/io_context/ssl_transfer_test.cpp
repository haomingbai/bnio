#include "ssl_test_support.h"

namespace {

void test_socketpair_read_write_transfers_plaintext() {
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

  {
    bupp::io_context context;
    if (!context.is_open()) {
      return;
    }
    auto scheduler = context.get_post_scheduler();

    auto state = std::make_shared<handshake_state>();
    handshake_receiver client_receiver{state, &context};
    handshake_receiver server_receiver{state, &context};

    auto client_sender =
        client.async_handshake(scheduler, bupp::ssl_handshake_type::client);
    auto server_sender =
        server.async_handshake(scheduler, bupp::ssl_handshake_type::server);

    auto client_operation =
        bexec::connect(std::move(client_sender), std::move(client_receiver));
    auto server_operation =
        bexec::connect(std::move(server_sender), std::move(server_receiver));

    bexec::start(client_operation);
    bexec::start(server_operation);
    context.run();

    assert(state->values == 2);
    assert(state->errors == 0);
    assert(state->stopped == 0);
  }

#if defined(BUPP_HAS_IO_CONTEXT_BSD)
  // Keep one application write below the conservative AF_UNIX socket buffer
  // size. The test waits for that write before issuing the next SSL read.
  constexpr std::size_t payload_size = 8 * 1024;
#else
  constexpr std::size_t payload_size = 70 * 1024;
#endif
  std::vector<unsigned char> payload(payload_size);
  for (std::size_t index = 0; index < payload.size(); ++index) {
    payload[index] = static_cast<unsigned char>((index * 17U + 29U) & 0xffU);
  }
  std::vector<unsigned char> received(payload.size());

  std::size_t sent = 0;
  std::size_t received_size = 0;
  while (received_size < payload.size()) {
    bupp::io_context context;
    if (!context.is_open()) {
      return;
    }
    auto scheduler = context.get_post_scheduler();

    unsigned completions = 0;
    auto read_state = std::make_shared<transfer_state>();
    auto write_state = std::make_shared<transfer_state>();
    const unsigned target = sent < payload.size() ? 2U : 1U;
    transfer_receiver read_receiver{read_state, &context, &completions, target};
    transfer_receiver write_receiver{write_state, &context, &completions,
                                     target};

    auto read_buffer = bupp::buffer(received.data() + received_size,
                                    received.size() - received_size);
    auto read_sender = server.async_read(scheduler, read_buffer);

    if (sent < payload.size()) {
      auto write_buffer =
          bupp::buffer(payload.data() + sent, payload.size() - sent);
      auto write_sender =
          client.async_write(scheduler, write_buffer, MSG_NOSIGNAL);

      auto read_operation =
          bexec::connect(std::move(read_sender), std::move(read_receiver));
      auto write_operation =
          bexec::connect(std::move(write_sender), std::move(write_receiver));

      bexec::start(read_operation);
      bexec::start(write_operation);
      if (completions != target) {
        context.run();
      }

      assert(write_state->values == 1);
      assert(write_state->errors == 0);
      assert(write_state->stopped == 0);
      assert(write_state->bytes == payload.size() - sent);
      sent += write_state->bytes;
    } else {
      auto read_operation =
          bexec::connect(std::move(read_sender), std::move(read_receiver));

      bexec::start(read_operation);
      if (completions != target) {
        context.run();
      }
    }

    assert(completions == target);
    assert(read_state->values == 1);
    assert(read_state->errors == 0);
    assert(read_state->stopped == 0);
    assert(read_state->bytes != 0);

    received_size += read_state->bytes;
  }

  assert(sent == payload.size());
  assert(received_size == payload.size());
  assert(std::memcmp(received.data(), payload.data(), payload.size()) == 0);

  test_require(!client.lowest_layer().close());
  bupp::io_context error_context;
  if (!error_context.is_open()) {
    return;
  }
  auto error_scheduler = error_context.get_post_scheduler();
  std::array<unsigned char, 1> trailing_byte{};
  unsigned completions = 0;
  auto error_state = std::make_shared<transfer_state>();
  auto read_operation = bexec::connect(
      server.async_read(error_scheduler, trailing_byte),
      transfer_receiver{error_state, &error_context, &completions, 1});
  bexec::start(read_operation);
  error_context.run();

  assert(completions == 1);
  assert(error_state->values == 0);
  assert(error_state->errors == 1);
  assert(error_state->stopped == 0);
  assert(error_state->error == std::errc::connection_reset);
}

}  // namespace

int main() {
  test_socketpair_read_write_transfers_plaintext();
  return 0;
}
