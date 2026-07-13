#include <cerrno>

#include "io_context_runtime_test_support.h"

namespace {

struct pair_byte_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bupp::io_context* context = nullptr;
  unsigned* completions = nullptr;
  unsigned target = 1;

  void set_value(std::size_t size) noexcept {
    state->signal = signal_kind::value;
    state->size = size;
    complete();
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    complete();
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    complete();
  }

 private:
  void complete() noexcept {
    if (completions != nullptr) {
      ++*completions;
      if (*completions != target) {
        return;
      }
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

void test_ready_socket_read_completes_without_queue() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  bupp::tcp_socket receiver_socket(sockets[0]);
  bupp::tcp_socket sender_socket(sockets[1]);

  constexpr std::string_view payload = "ready";
  assert(::send(sender_socket.native_handle(), payload.data(), payload.size(),
                MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size()));

  std::array<char, 16> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = receiver_socket.async_read(scheduler, bupp::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->size == payload.size());
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

void test_passive_drain_reads_io() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  bupp::tcp_socket receiver_socket(sockets[0]);
  bupp::tcp_socket sender_socket(sockets[1]);

  std::array<char, 16> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = receiver_socket.async_read(scheduler, bupp::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  constexpr std::string_view payload = "passive";
  assert(::send(sender_socket.native_handle(), payload.data(), payload.size(),
                MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size()));

  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->size == payload.size());
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

void test_ready_socket_write_completes_without_queue() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  bupp::tcp_socket sender_socket(sockets[0]);
  bupp::tcp_socket receiver_socket(sockets[1]);

  constexpr std::string_view payload = "ready write";
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender =
      sender_socket.async_write(scheduler, bupp::buffer(payload), MSG_NOSIGNAL);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->size == payload.size());

  std::array<char, 32> bytes{};
  assert(::recv(receiver_socket.native_handle(), bytes.data(), bytes.size(),
                0) == static_cast<ssize_t>(payload.size()));
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

void test_blocked_socket_write_falls_back_to_queue() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  bupp::tcp_socket sender_socket(sockets[0]);
  bupp::tcp_socket receiver_socket(sockets[1]);

  std::array<char, 4096> filler{};
  while (true) {
    const ssize_t result = ::send(sender_socket.native_handle(), filler.data(),
                                  filler.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (result > 0) {
      continue;
    }
    assert(result < 0);
    if (errno == EINTR) {
      continue;
    }
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    break;
  }

  constexpr std::string_view payload = "fallback write";
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;
  auto sender = sender_socket.async_write_some(scheduler, bupp::buffer(payload),
                                               MSG_NOSIGNAL);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  std::array<char, 65536> drain{};
  while (true) {
    const ssize_t result = ::recv(receiver_socket.native_handle(), drain.data(),
                                  drain.size(), MSG_DONTWAIT);
    if (result > 0) {
      continue;
    }
    assert(result < 0);
    if (errno == EINTR) {
      continue;
    }
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    break;
  }

  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->size == payload.size());
  std::array<char, 32> bytes{};
  assert(::recv(receiver_socket.native_handle(), bytes.data(), bytes.size(),
                0) == static_cast<ssize_t>(payload.size()));
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

void test_io_idle_drain_reads() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  bupp::tcp_socket receiver_socket(sockets[0]);
  bupp::tcp_socket sender_socket(sockets[1]);

  std::array<char, 16> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = receiver_socket.async_read(scheduler, bupp::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  constexpr std::string_view payload = "auto";
  assert(::send(sender_socket.native_handle(), payload.data(), payload.size(),
                MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size()));

  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->size == payload.size());
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

void test_io_idle_drain_read_write_pair() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  bupp::tcp_socket receiver_socket(sockets[0]);
  bupp::tcp_socket sender_socket(sockets[1]);

  constexpr std::string_view payload = "auto pair";
  std::array<char, 32> bytes{};
  unsigned completions = 0;

  pair_byte_receiver read_receiver;
  read_receiver.context = &context;
  read_receiver.completions = &completions;
  read_receiver.target = 2;
  auto read_state = read_receiver.state;

  pair_byte_receiver write_receiver;
  write_receiver.context = &context;
  write_receiver.completions = &completions;
  write_receiver.target = 2;
  auto write_state = write_receiver.state;

  auto read_sender = receiver_socket.async_read(scheduler, bupp::buffer(bytes));
  auto write_sender =
      sender_socket.async_write(scheduler, bupp::buffer(payload), MSG_NOSIGNAL);
  auto read_operation =
      bexec::connect(std::move(read_sender), std::move(read_receiver));
  auto write_operation =
      bexec::connect(std::move(write_sender), std::move(write_receiver));

  bexec::start(read_operation);
  bexec::start(write_operation);
  context.run();

  assert(completions == 2);
  assert(read_state->signal == signal_kind::value);
  assert(read_state->size == payload.size());
  assert(write_state->signal == signal_kind::value);
  assert(write_state->size == payload.size());
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

void test_file_write_and_read() {
  std::string path = "/tmp/bupp-io-context-file-XXXXXX";
  const int fd = ::mkstemp(path.data());
  assert(fd >= 0);
  assert(::unlink(path.c_str()) == 0);

  constexpr std::string_view payload = "file through io_uring";

  {
    bupp::io_context context;
    if (!context_available(context)) {
      assert(::close(fd) == 0);
      return;
    }
    auto scheduler = context.get_post_scheduler();

    byte_receiver receiver;
    receiver.context = &context;
    auto state = receiver.state;

    auto sender =
        bupp::async_write(scheduler, bupp::async_io::descriptor_view(fd),
                          bupp::buffer(payload), 0);
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    context.run();

    assert(state->signal == signal_kind::value);
    assert(state->size == payload.size());
  }

  {
    bupp::io_context context;
    if (!context_available(context)) {
      assert(::close(fd) == 0);
      return;
    }
    auto scheduler = context.get_post_scheduler();
    std::array<char, 64> bytes{};

    byte_receiver receiver;
    receiver.context = &context;
    auto state = receiver.state;

    auto sender = bupp::async_read(
        scheduler, bupp::async_io::descriptor_view(fd), bupp::buffer(bytes), 0);
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    context.run();

    assert(state->signal == signal_kind::value);
    assert(state->size == payload.size());
    assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
  }

  assert(::close(fd) == 0);
}

}  // namespace

int main() {
  test_ready_socket_read_completes_without_queue();
  test_passive_drain_reads_io();
  test_ready_socket_write_completes_without_queue();
  test_blocked_socket_write_falls_back_to_queue();
  test_io_idle_drain_reads();
  test_io_idle_drain_read_write_pair();
  test_file_write_and_read();
  return 0;
}
