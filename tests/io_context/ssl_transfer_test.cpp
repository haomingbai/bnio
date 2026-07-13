#include "ssl_test_support.h"

namespace {

template <bool DirectSubmit>
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
    context.run();

    assert(state->values == 2);
    assert(state->errors == 0);
    assert(state->stopped == 0);
  }

  std::vector<unsigned char> payload(70 * 1024);
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
    auto read_sender = [&] {
      if constexpr (DirectSubmit) {
        return server.async_read_direct(scheduler, read_buffer);
      } else {
        return server.async_read(scheduler, read_buffer);
      }
    }();

    if (sent < payload.size()) {
      auto write_buffer =
          bupp::buffer(payload.data() + sent, payload.size() - sent);
      auto write_sender = [&] {
        if constexpr (DirectSubmit) {
          return client.async_write_direct(scheduler, write_buffer,
                                           MSG_NOSIGNAL);
        } else {
          return client.async_write(scheduler, write_buffer, MSG_NOSIGNAL);
        }
      }();

      auto read_operation =
          bexec::connect(std::move(read_sender), std::move(read_receiver));
      auto write_operation =
          bexec::connect(std::move(write_sender), std::move(write_receiver));

      bexec::start(read_operation);
      bexec::start(write_operation);
      if constexpr (DirectSubmit) {
        assert(scheduler.queued_io_size() == 0);
      }
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
      if constexpr (DirectSubmit) {
        assert(scheduler.queued_io_size() == 0);
      }
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
}

}  // namespace

int main() {
  test_socketpair_read_write_transfers_plaintext<false>();
  test_socketpair_read_write_transfers_plaintext<true>();
  return 0;
}
