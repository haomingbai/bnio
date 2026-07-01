#include <arpa/inet.h>
#include <bupp/io_context.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <bexec/operation_state.hpp>
#include <bexec/scheduler.hpp>
#include <bexec/sender.hpp>
#include <bexec/stop_token.hpp>
#include <cassert>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
  int fd = -1;
  std::error_code error;
};

struct schedule_state {
  signal_kind signal = signal_kind::none;
  std::vector<int> order;
  unsigned completions = 0;
  bool completed_during_start = false;
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

struct socket_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bupp::io_context* context = nullptr;
  unsigned* completions = nullptr;
  unsigned target = 1;

  void set_value(bupp::tcp_socket socket) noexcept {
    state->signal = signal_kind::value;
    state->fd = socket.release();
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

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
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

struct void_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bupp::io_context* context = nullptr;
  unsigned* completions = nullptr;
  unsigned target = 1;

  void set_value() noexcept {
    state->signal = signal_kind::value;
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

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
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

struct poll_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bupp::io_context* context = nullptr;

  void set_value(unsigned events) noexcept {
    state->signal = signal_kind::value;
    state->size = events;
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

struct stop_env {
  bexec::inplace_stop_token token;

  [[nodiscard]] bexec::inplace_stop_token query(
      bexec::get_stop_token_t) const noexcept {
    return token;
  }
};

struct stopped_void_receiver : void_receiver {
  stop_env env;

  [[nodiscard]] stop_env get_env() const noexcept { return env; }
};

struct schedule_receiver {
  std::shared_ptr<schedule_state> state = std::make_shared<schedule_state>();
  bupp::io_context* context = nullptr;
  int value = 0;
  unsigned target = 1;

  void set_value() noexcept {
    state->signal = signal_kind::value;
    state->order.push_back(value);
    ++state->completions;
    if (context != nullptr && state->completions == target) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    (void)error;
    state->signal = signal_kind::error;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    ++state->completions;
    if (context != nullptr && state->completions == target) {
      (void)context->stop();
    }
  }
};

struct stopped_schedule_receiver : schedule_receiver {
  stop_env env;

  [[nodiscard]] stop_env get_env() const noexcept { return env; }
};

struct dispatch_inline_outer_receiver {
  std::shared_ptr<schedule_state> state;
  bupp::io_context* context = nullptr;

  void set_value() noexcept {
    schedule_receiver inner;
    inner.state = state;
    inner.value = 42;

    auto sender = bexec::schedule(context->get_dispatch_scheduler());
    auto operation = bexec::connect(std::move(sender), std::move(inner));
    bexec::start(operation);

    state->completed_during_start = state->signal == signal_kind::value;
    (void)context->stop();
  }

  void set_error(std::error_code error) noexcept {
    (void)error;
    state->signal = signal_kind::error;
    (void)context->stop();
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    (void)context->stop();
  }
};

template <class Scheduler, class Stream, class Buffer>
concept scheduler_can_receive_stream =
    requires(Scheduler scheduler, Stream stream, Buffer buffer) {
      scheduler.async_receive(stream, buffer);
    };

template <class Scheduler, class Stream, class Buffer>
concept scheduler_can_send_stream =
    requires(Scheduler scheduler, Stream stream, Buffer buffer) {
      scheduler.async_send(stream, buffer);
    };

[[nodiscard]] bool context_available(const bupp::io_context& context) {
  return context.is_open();
}

[[nodiscard]] bupp::ip::endpoint bound_loopback_endpoint(
    const bupp::tcp_acceptor& acceptor) {
  sockaddr_in address{};
  socklen_t address_size = sizeof(address);
  assert(::getsockname(acceptor.native_handle(),
                       reinterpret_cast<sockaddr*>(&address),
                       &address_size) == 0);
  assert(address.sin_family == AF_INET);
  return bupp::ip::endpoint(bupp::ip::address::loopback_v4(),
                            ntohs(address.sin_port));
}

void test_sender_concepts() {
  bupp::io_context context;
  auto scheduler = context.get_post_scheduler();
  bupp::tcp_socket socket(3);
  std::array<char, 8> bytes{};
  constexpr std::string_view text = "abc";

  using receive_sender =
      decltype(socket.async_receive(scheduler, bupp::buffer(bytes)));
  using receive_direct_sender =
      decltype(socket.async_receive_direct(scheduler, bupp::buffer(bytes)));
  using low_receive_sender =
      decltype(scheduler.async_receive(socket.view(), bupp::buffer(bytes)));
  using low_send_sender =
      decltype(scheduler.async_send(socket.view(), bupp::buffer(text)));
  using send_sender = decltype(socket.async_send(scheduler, text));
  using send_direct_sender =
      decltype(socket.async_send_direct(scheduler, text));
  using accept_sender =
      decltype(std::declval<bupp::tcp_acceptor&>().async_accept(scheduler));
  using accept_direct_sender =
      decltype(std::declval<bupp::tcp_acceptor&>().async_accept_direct(
          scheduler));
  using connect_sender =
      decltype(std::declval<bupp::tcp_socket&>().async_connect(
          scheduler, std::declval<const bupp::ip::endpoint&>()));
  using connect_direct_sender =
      decltype(std::declval<bupp::tcp_socket&>().async_connect_direct(
          scheduler, std::declval<const bupp::ip::endpoint&>()));
  using read_sender = decltype(scheduler.async_read(
      bupp::async_io::descriptor_view(3), bupp::buffer(bytes)));
  using read_direct_sender = decltype(scheduler.async_read_direct(
      bupp::async_io::descriptor_view(3), bupp::buffer(bytes)));
  using write_sender = decltype(scheduler.async_write(
      bupp::async_io::descriptor_view(3), bupp::buffer(text)));
  using write_direct_sender = decltype(scheduler.async_write_direct(
      bupp::async_io::descriptor_view(3), bupp::buffer(text)));
  using poll_sender = decltype(scheduler.async_poll(
      bupp::async_io::descriptor_view(3), static_cast<unsigned>(POLLIN)));
  using schedule_sender = decltype(bexec::schedule(scheduler));
  using timer_wait_sender =
      decltype(std::declval<bupp::steady_timer&>().async_wait());
  static_assert(bexec::sender<receive_sender>);
  static_assert(bexec::sender<receive_direct_sender>);
  static_assert(bexec::sender<low_receive_sender>);
  static_assert(bexec::sender<low_send_sender>);
  static_assert(bexec::sender<send_sender>);
  static_assert(bexec::sender<send_direct_sender>);
  static_assert(bexec::sender<accept_sender>);
  static_assert(bexec::sender<accept_direct_sender>);
  static_assert(bexec::sender<connect_sender>);
  static_assert(bexec::sender<connect_direct_sender>);
  static_assert(bexec::sender<read_sender>);
  static_assert(bexec::sender<read_direct_sender>);
  static_assert(bexec::sender<write_sender>);
  static_assert(bexec::sender<write_direct_sender>);
  static_assert(bexec::sender<poll_sender>);
  static_assert(bexec::sender<schedule_sender>);
  static_assert(bexec::sender<timer_wait_sender>);
  static_assert(bexec::scheduler<bupp::io_context::dispatch_scheduler>);
  static_assert(bexec::scheduler<bupp::io_context::post_scheduler>);
  static_assert(
      !scheduler_can_receive_stream<bupp::io_context::post_scheduler,
                                    bupp::tcp_socket, bupp::mutable_buffer>);
  static_assert(
      !scheduler_can_send_stream<bupp::io_context::post_scheduler,
                                 bupp::tcp_socket, bupp::const_buffer>);
  static_assert(bupp::receives_bytes<bupp::io_context::post_scheduler,
                                     bupp::tcp_socket, bupp::mutable_buffer>);
  static_assert(bupp::receives_bytes<bupp::io_context::dispatch_scheduler,
                                     bupp::tcp_socket, bupp::mutable_buffer>);
  static_assert(bupp::sends_bytes<bupp::io_context::post_scheduler,
                                  bupp::tcp_socket, bupp::const_buffer>);
  static_assert(bupp::sends_bytes<bupp::io_context::dispatch_scheduler,
                                  bupp::tcp_socket, bupp::const_buffer>);
  static_assert(bupp::accepts_connections<bupp::io_context::post_scheduler,
                                          bupp::tcp_acceptor>);
  static_assert(
      bupp::connects_stream<bupp::io_context::post_scheduler, bupp::tcp_socket,
                            const bupp::ip::endpoint&>);
  static_assert(bupp::reads_descriptor<bupp::io_context::post_scheduler,
                                       bupp::async_io::descriptor_view,
                                       bupp::mutable_buffer>);
  static_assert(bupp::writes_descriptor<bupp::io_context::post_scheduler,
                                        bupp::async_io::descriptor_view,
                                        bupp::const_buffer>);
  static_assert(bupp::polls_descriptor<bupp::io_context::post_scheduler,
                                       bupp::async_io::descriptor_view>);

  byte_receiver receiver;
  auto sender = socket.async_receive(scheduler, bupp::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  static_assert(bexec::operation_state<decltype(operation)>);

  (void)socket.release();
}

void test_queued_poll_observes_pipe_readiness() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int descriptors[2] = {-1, -1};
  assert(::pipe2(descriptors, O_CLOEXEC) == 0);

  poll_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = bupp::async_poll(
      scheduler, bupp::async_io::descriptor_view(descriptors[0]),
      static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  assert(scheduler.queued_io_size() == 1);

  constexpr char byte = 'q';
  assert(::write(descriptors[1], &byte, sizeof(byte)) ==
         static_cast<ssize_t>(sizeof(byte)));
  assert(!scheduler.flush_io_queue());
  context.run();

  assert(state->signal == signal_kind::value);
  assert((static_cast<unsigned>(state->size) & static_cast<unsigned>(POLLIN)) !=
         0);

  assert(::close(descriptors[0]) == 0);
  assert(::close(descriptors[1]) == 0);
}

void test_direct_poll_submits_without_queue() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int descriptors[2] = {-1, -1};
  assert(::pipe2(descriptors, O_CLOEXEC) == 0);

  poll_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = bupp::async_poll_direct(
      scheduler, bupp::async_io::descriptor_view(descriptors[0]),
      static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  assert(scheduler.queued_io_size() == 0);

  constexpr char byte = 'd';
  assert(::write(descriptors[1], &byte, sizeof(byte)) ==
         static_cast<ssize_t>(sizeof(byte)));
  context.run();

  assert(state->signal == signal_kind::value);
  assert((static_cast<unsigned>(state->size) & static_cast<unsigned>(POLLIN)) !=
         0);

  assert(::close(descriptors[0]) == 0);
  assert(::close(descriptors[1]) == 0);
}

void test_manual_flush_receives_queued_io() {
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

  auto sender = receiver_socket.async_receive(scheduler, bupp::buffer(bytes));
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

void test_direct_receive_submits_without_queue() {
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
      receiver_socket.async_receive_direct(scheduler, bupp::buffer(bytes));
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

void test_queued_send_writes_to_peer() {
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

  constexpr std::string_view payload = "queued send";
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender =
      sender_socket.async_send(scheduler, bupp::buffer(payload), MSG_NOSIGNAL);
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

void test_direct_send_submits_without_queue() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  bupp::tcp_socket sender_socket(sockets[0]);
  bupp::tcp_socket receiver_socket(sockets[1]);

  constexpr std::string_view payload = "direct send";
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = sender_socket.async_send_direct(
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

template <bool Direct>
void test_accept_connect_loopback() {
  bupp::io_context_options options;
  options.platform.max_queued_io_operations = 64;
  options.platform.queued_io_flush_after = std::chrono::seconds(30);
  bupp::io_context context(options);
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

  auto accept_sender = [&] {
    if constexpr (Direct) {
      return acceptor.async_accept_direct(scheduler, SOCK_CLOEXEC);
    } else {
      return acceptor.async_accept(scheduler, SOCK_CLOEXEC);
    }
  }();
  auto connect_sender = [&] {
    if constexpr (Direct) {
      return client.async_connect_direct(scheduler, endpoint);
    } else {
      return client.async_connect(scheduler, endpoint);
    }
  }();

  auto accept_operation =
      bexec::connect(std::move(accept_sender), std::move(accept_receiver));
  auto connect_operation =
      bexec::connect(std::move(connect_sender), std::move(connect_receiver));

  bexec::start(accept_operation);
  bexec::start(connect_operation);
  if constexpr (Direct) {
    assert(scheduler.queued_io_size() == 0);
  } else {
    assert(scheduler.queued_io_size() == 2);
    assert(!scheduler.flush_io_queue());
  }
  context.run();

  assert(completions == 2);
  assert(accept_state->signal == signal_kind::value);
  assert(accept_state->fd >= 0);
  assert(connect_state->signal == signal_kind::value);
  assert(client.is_open());

  assert(::close(accept_state->fd) == 0);
  accept_state->fd = -1;
}

void test_queued_accept_connect_loopback() {
  test_accept_connect_loopback<false>();
}

void test_direct_accept_connect_loopback() {
  test_accept_connect_loopback<true>();
}

void test_queued_io_auto_flush_timer_receives() {
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

  auto sender = receiver_socket.async_receive(scheduler, bupp::buffer(bytes));
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

void test_post_scheduler_schedule_posts_fifo() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  auto scheduler = context.get_post_scheduler();
  auto state = std::make_shared<schedule_state>();
  state->order.reserve(3);

  schedule_receiver first;
  first.state = state;
  first.context = &context;
  first.value = 1;
  first.target = 3;

  schedule_receiver second;
  second.state = state;
  second.context = &context;
  second.value = 2;
  second.target = 3;

  schedule_receiver third;
  third.state = state;
  third.context = &context;
  third.value = 3;
  third.target = 3;

  auto first_operation =
      bexec::connect(bexec::schedule(scheduler), std::move(first));
  auto second_operation =
      bexec::connect(bexec::schedule(scheduler), std::move(second));
  auto third_operation =
      bexec::connect(bexec::schedule(scheduler), std::move(third));

  bexec::start(first_operation);
  bexec::start(second_operation);
  bexec::start(third_operation);

  assert(state->order.empty());
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->order.size() == 3);
  assert(state->order[0] == 1);
  assert(state->order[1] == 2);
  assert(state->order[2] == 3);
}

void test_dispatch_scheduler_schedule_posts_outside_context() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  auto scheduler = context.get_dispatch_scheduler();
  auto state = std::make_shared<schedule_state>();
  state->order.reserve(1);

  schedule_receiver receiver;
  receiver.state = state;
  receiver.context = &context;
  receiver.value = 7;

  auto operation =
      bexec::connect(bexec::schedule(scheduler), std::move(receiver));
  bexec::start(operation);

  assert(state->signal == signal_kind::none);
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->order.size() == 1);
  assert(state->order[0] == 7);
}

void test_dispatch_scheduler_schedule_runs_inline_in_context() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  auto state = std::make_shared<schedule_state>();
  state->order.reserve(1);

  auto operation =
      bexec::connect(bexec::schedule(context.get_post_scheduler()),
                     dispatch_inline_outer_receiver{state, &context});
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->completed_during_start);
  assert(state->order.size() == 1);
  assert(state->order[0] == 42);
}

void test_scheduler_schedule_pre_stopped_token_stops() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  bexec::inplace_stop_source source;
  assert(source.request_stop());

  auto state = std::make_shared<schedule_state>();
  stopped_schedule_receiver receiver;
  receiver.state = state;
  receiver.context = &context;
  receiver.env = stop_env{source.get_token()};

  auto operation = bexec::connect(bexec::schedule(context.get_post_scheduler()),
                                  std::move(receiver));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::stopped);
}

void test_steady_timer_completes() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  bupp::steady_timer timer(context);
  assert(timer.expires_after(std::chrono::milliseconds(1)) == 0);

  void_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = timer.async_wait();
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::value);
}

void test_steady_timer_cancel_stops_wait() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  bupp::steady_timer timer(context);
  assert(timer.expires_after(std::chrono::seconds(30)) == 0);

  void_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = timer.async_wait();
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  assert(timer.cancel() == 1);
  context.run();

  assert(state->signal == signal_kind::stopped);
}

void test_steady_timer_expires_after_stops_old_wait() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  bupp::steady_timer timer(context);
  assert(timer.expires_after(std::chrono::seconds(30)) == 0);

  void_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = timer.async_wait();
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  assert(timer.expires_after(std::chrono::milliseconds(1)) == 1);
  context.run();

  assert(state->signal == signal_kind::stopped);
}

void test_steady_timer_multiple_waits_complete() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  bupp::steady_timer timer(context);
  assert(timer.expires_after(std::chrono::milliseconds(1)) == 0);

  unsigned completions = 0;
  void_receiver first;
  first.context = &context;
  first.completions = &completions;
  first.target = 2;
  auto first_state = first.state;

  void_receiver second;
  second.context = &context;
  second.completions = &completions;
  second.target = 2;
  auto second_state = second.state;

  auto first_sender = timer.async_wait();
  auto second_sender = timer.async_wait();
  auto first_operation =
      bexec::connect(std::move(first_sender), std::move(first));
  auto second_operation =
      bexec::connect(std::move(second_sender), std::move(second));
  bexec::start(first_operation);
  bexec::start(second_operation);
  context.run();

  assert(completions == 2);
  assert(first_state->signal == signal_kind::value);
  assert(second_state->signal == signal_kind::value);
}

void test_steady_timer_move_stops_old_wait() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  bupp::steady_timer timer(context);
  assert(timer.expires_after(std::chrono::seconds(30)) == 0);

  unsigned completions = 0;
  void_receiver receiver;
  receiver.context = &context;
  receiver.completions = &completions;
  receiver.target = 2;
  auto state = receiver.state;

  auto sender = timer.async_wait();
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  bupp::steady_timer moved_timer(std::move(timer));
  assert(moved_timer.expires_after(std::chrono::milliseconds(1)) == 0);

  void_receiver moved_receiver;
  moved_receiver.context = &context;
  moved_receiver.completions = &completions;
  moved_receiver.target = 2;
  auto moved_state = moved_receiver.state;
  auto moved_sender = moved_timer.async_wait();
  auto moved_operation =
      bexec::connect(std::move(moved_sender), std::move(moved_receiver));
  bexec::start(moved_operation);
  context.run();

  assert(completions == 2);
  assert(state->signal == signal_kind::stopped);
  assert(moved_state->signal == signal_kind::value);
}

void test_steady_timer_pre_stopped_token_stops_wait() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  bexec::inplace_stop_source source;
  assert(source.request_stop());
  bupp::steady_timer timer(context);
  assert(timer.expires_after(std::chrono::seconds(30)) == 0);

  stopped_void_receiver receiver;
  receiver.context = &context;
  receiver.env = stop_env{source.get_token()};
  auto state = receiver.state;

  auto sender = timer.async_wait();
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::stopped);
}

}  // namespace

int main() {
  test_sender_concepts();
  test_queued_poll_observes_pipe_readiness();
  test_direct_poll_submits_without_queue();
  test_manual_flush_receives_queued_io();
  test_direct_receive_submits_without_queue();
  test_queued_send_writes_to_peer();
  test_direct_send_submits_without_queue();
  test_queued_accept_connect_loopback();
  test_direct_accept_connect_loopback();
  test_queued_io_auto_flush_timer_receives();
  test_queued_file_write_and_direct_read();
  test_direct_file_write_and_queued_read();
  test_post_scheduler_schedule_posts_fifo();
  test_dispatch_scheduler_schedule_posts_outside_context();
  test_dispatch_scheduler_schedule_runs_inline_in_context();
  test_scheduler_schedule_pre_stopped_token_stops();
  test_steady_timer_completes();
  test_steady_timer_cancel_stops_wait();
  test_steady_timer_expires_after_stops_old_wait();
  test_steady_timer_multiple_waits_complete();
  test_steady_timer_move_stops_old_wait();
  test_steady_timer_pre_stopped_token_stops_wait();
  return 0;
}
