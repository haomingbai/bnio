#include <bupp/io_context.h>
#include <sys/socket.h>

#include <array>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cassert>
#include <cstring>
#include <memory>
#include <string_view>
#include <system_error>

namespace {

enum class signal_kind {
  none,
  value,
  error,
  stopped,
};

struct shared_state {
  signal_kind signal = signal_kind::none;
  std::size_t size = 0;
  std::error_code error;
};

struct byte_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bupp::io_context* context = nullptr;

  void set_value(std::size_t size) noexcept {
    state->signal = signal_kind::value;
    state->size = size;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

[[nodiscard]] bool context_available(const bupp::io_context& context) {
  return context.is_open();
}

void test_sender_concepts() {
  bupp::io_context context;
  bupp::tcp_socket socket(3);
  std::array<char, 8> bytes{};

  using receive_sender =
      decltype(context.async_receive(socket, bupp::buffer(bytes)));
  using send_sender =
      decltype(context.async_send(socket, std::string_view("abc")));
  static_assert(bexec::sender<receive_sender>);
  static_assert(bexec::sender<send_sender>);
  static_assert(bupp::receives_bytes<bupp::io_context, bupp::tcp_socket,
                                     bupp::mutable_buffer>);
  static_assert(bupp::sends_bytes<bupp::io_context, bupp::tcp_socket,
                                  bupp::const_buffer>);

  byte_receiver receiver;
  auto sender = context.async_receive(socket, bupp::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  static_assert(bexec::operation_state<decltype(operation)>);

  (void)socket.release();
}

void test_manual_flush_receives_queued_io() {
  bupp::io_context_options options;
  options.platform.max_queued_io_operations = 64;
  options.platform.queued_io_flush_after = std::chrono::seconds(30);
  bupp::io_context context(options);
  if (!context_available(context)) {
    return;
  }

  int sockets[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  bupp::tcp_socket receiver_socket(sockets[0]);
  bupp::tcp_socket sender_socket(sockets[1]);

  std::array<char, 16> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = context.async_receive(receiver_socket, bupp::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  assert(context.queued_io_size() == 1);

  constexpr std::string_view payload = "queued";
  assert(::send(sender_socket.native_handle(), payload.data(), payload.size(),
                MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size()));

  assert(!context.flush_io_queue());
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->size == payload.size());
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

void test_direct_receive_submits_without_queue() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  int sockets[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  bupp::tcp_socket receiver_socket(sockets[0]);
  bupp::tcp_socket sender_socket(sockets[1]);

  std::array<char, 16> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender =
      context.async_receive_direct(receiver_socket, bupp::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  assert(context.queued_io_size() == 0);

  constexpr std::string_view payload = "direct";
  assert(::send(sender_socket.native_handle(), payload.data(), payload.size(),
                MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size()));

  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->size == payload.size());
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

}  // namespace

int main() {
  test_sender_concepts();
  test_manual_flush_receives_queued_io();
  test_direct_receive_submits_without_queue();
  return 0;
}
