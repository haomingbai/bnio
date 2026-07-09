#include "io_context_runtime_test_support.h"

namespace {

void test_manual_flush_reads_queued_io() {
  bupp::io_context_options options;
  options.platform.max_queued_io_operations = 64;
  options.platform.queued_io_flush_after = std::chrono::seconds(30);
  bupp::io_context context(options);
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
  assert(scheduler.queued_io_size() == 1);

  constexpr std::string_view payload = "queued";
  assert(::send(sender_socket.native_handle(), payload.data(), payload.size(),
                MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size()));

  assert(!scheduler.flush_io_queue());
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->size == payload.size());
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

void test_direct_read_submits_without_queue() {
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

  auto sender =
      receiver_socket.async_read_direct(scheduler, bupp::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  assert(scheduler.queued_io_size() == 0);

  constexpr std::string_view payload = "direct";
  assert(::send(sender_socket.native_handle(), payload.data(), payload.size(),
                MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size()));

  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->size == payload.size());
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

void test_queued_write_writes_to_peer() {
  bupp::io_context_options options;
  options.platform.max_queued_io_operations = 64;
  options.platform.queued_io_flush_after = std::chrono::seconds(30);
  bupp::io_context context(options);
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  bupp::tcp_socket sender_socket(sockets[0]);
  bupp::tcp_socket receiver_socket(sockets[1]);

  constexpr std::string_view payload = "queued write";
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender =
      sender_socket.async_write(scheduler, bupp::buffer(payload), MSG_NOSIGNAL);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  assert(scheduler.queued_io_size() == 1);
  assert(!scheduler.flush_io_queue());
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->size == payload.size());

  std::array<char, 32> bytes{};
  assert(::recv(receiver_socket.native_handle(), bytes.data(), bytes.size(),
                0) == static_cast<ssize_t>(payload.size()));
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

void test_direct_write_submits_without_queue() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  bupp::tcp_socket sender_socket(sockets[0]);
  bupp::tcp_socket receiver_socket(sockets[1]);

  constexpr std::string_view payload = "direct write";
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = sender_socket.async_write_direct(
      scheduler, bupp::buffer(payload), MSG_NOSIGNAL);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  assert(scheduler.queued_io_size() == 0);
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->size == payload.size());

  std::array<char, 32> bytes{};
  assert(::recv(receiver_socket.native_handle(), bytes.data(), bytes.size(),
                0) == static_cast<ssize_t>(payload.size()));
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

void test_queued_io_auto_flush_timer_reads() {
  bupp::io_context_options options;
  options.platform.max_queued_io_operations = 64;
  options.platform.queued_io_flush_after = std::chrono::milliseconds(1);
  bupp::io_context context(options);
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
  assert(scheduler.queued_io_size() == 1);

  constexpr std::string_view payload = "auto";
  assert(::send(sender_socket.native_handle(), payload.data(), payload.size(),
                MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size()));

  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->size == payload.size());
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

void test_queued_file_write_and_direct_read() {
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
    assert(scheduler.queued_io_size() == 1);
    assert(!scheduler.flush_io_queue());
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

    auto sender = bupp::async_read_direct(
        scheduler, bupp::async_io::descriptor_view(fd), bupp::buffer(bytes), 0);
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    assert(scheduler.queued_io_size() == 0);
    context.run();

    assert(state->signal == signal_kind::value);
    assert(state->size == payload.size());
    assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
  }

  assert(::close(fd) == 0);
}

void test_direct_file_write_and_queued_read() {
  std::string path = "/tmp/bupp-io-context-file-XXXXXX";
  const int fd = ::mkstemp(path.data());
  assert(fd >= 0);
  assert(::unlink(path.c_str()) == 0);

  constexpr std::string_view payload = "direct file write";

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
        bupp::async_write_direct(scheduler, bupp::async_io::descriptor_view(fd),
                                 bupp::buffer(payload), 0);
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    assert(scheduler.queued_io_size() == 0);
    context.run();

    assert(state->signal == signal_kind::value);
    assert(state->size == payload.size());
  }

  {
    bupp::io_context_options options;
    options.platform.max_queued_io_operations = 64;
    options.platform.queued_io_flush_after = std::chrono::seconds(30);
    bupp::io_context context(options);
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
    assert(scheduler.queued_io_size() == 1);
    assert(!scheduler.flush_io_queue());
    context.run();

    assert(state->signal == signal_kind::value);
    assert(state->size == payload.size());
    assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
  }

  assert(::close(fd) == 0);
}

}  // namespace

int main() {
  test_manual_flush_reads_queued_io();
  test_direct_read_submits_without_queue();
  test_queued_write_writes_to_peer();
  test_direct_write_submits_without_queue();
  test_queued_io_auto_flush_timer_reads();
  test_queued_file_write_and_direct_read();
  test_direct_file_write_and_queued_read();
  return 0;
}
