// Coverage for the eager / immediate-completion toggle (commit C).
//
// The eager switch is a runtime io_context option:
// `io_context_options.enable_immediate_io` (default true, immutable after
// construction). With eager on an operation first attempts one non-blocking
// syscall before registering readiness; with false it skips that probe and
// goes straight to readiness waiting (kqueue) or SQE submission (io_uring).
// Both modes must produce identical observable results, so each scenario
// below runs twice, once with eager on and once with eager off.
//
// On Linux accept/connect never had an immediate attempt, so the two modes are
// equivalent there; every test must pass on both the BSD kqueue and the Linux
// io_uring backends.

#include <arpa/inet.h>
#include <bnio/udp.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "../../support/io_context/io_context_loopback_test_support.h"
#include "../../support/io_context/io_context_runtime_test_support.h"

// SOCK_NONBLOCK is a Linux/Android socket() flag; BSD accepts it as an
// accept4() flag but does not define it. The kqueue accept path forces
// O_NONBLOCK on the accepted descriptor regardless, so a zero fallback keeps
// the flag argument harmless on BSD while Linux still requests it natively.
#if !defined(SOCK_NONBLOCK)
#define SOCK_NONBLOCK 0
#endif

namespace {

// Builds io_context_options selecting the eager mode for a scenario. The
// switch is immutable after io_context construction, so each helper creates
// its own context from these options and calls the scheduler factories
// without a template argument.
template <bool Eager>
[[nodiscard]] bnio::io_context_options eager_options() {
  bnio::io_context_options options;
  options.enable_immediate_io = Eager;
  return options;
}

struct pair_byte_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bnio::io_context* context = nullptr;
  unsigned* completions = nullptr;
  unsigned target = 1;

  void set_value(std::error_code ec, std::size_t size) noexcept {
    if (ec) {
      state->signal = signal_kind::error;
      state->error = ec;
    } else {
      state->signal = signal_kind::value;
      state->size = size;
    }
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

// Receiver for the scheduler-level async_accept sender, which completes with
// the raw accepted descriptor (ec, int) instead of a tcp_socket.
struct fd_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bnio::io_context* context = nullptr;

  void set_value(std::error_code ec, int fd) noexcept {
    if (ec) {
      state->signal = signal_kind::error;
      state->error = ec;
    } else {
      state->signal = signal_kind::value;
      state->fd = fd;
    }
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

struct stopped_byte_receiver : byte_receiver {
  stop_env env;

  [[nodiscard]] stop_env get_env() const noexcept { return env; }
};

// Opens a non-blocking loopback client and waits until it is really connected
// (or fails). Returns the connected descriptor, or -1 on failure.
[[nodiscard]] int connect_loopback_ready(std::uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  const int fd =
      ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    return -1;
  }
  const int rc =
      ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  if (rc == 0) {
    return fd;
  }
  if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EALREADY) {
    ::close(fd);
    return -1;
  }
  pollfd descriptor{fd, POLLOUT, 0};
  const int poll_result = ::poll(&descriptor, 1, 5000);
  if (poll_result <= 0) {
    ::close(fd);
    return -1;
  }
  int error = 0;
  socklen_t length = sizeof(error);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 ||
      error != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// Loopback TCP listener. The listener descriptor is forced to O_NONBLOCK:
// the BSD accept path never flips the listener itself, and a blocking accept
// with no pending connection would stall the eager probe.
struct loopback_listener {
  bnio::tcp_acceptor acceptor;
  std::uint16_t port = 0;

  bool setup() {
    if (acceptor.open(bnio::ip::tcp::v4())) {
      return false;
    }
    if (acceptor.set_reuse_address(true)) {
      return false;
    }
    if (acceptor.bind(bnio::ip::endpoint::loopback_v4(0))) {
      return false;
    }
    if (acceptor.listen(4)) {
      return false;
    }
    const int flags = ::fcntl(acceptor.native_handle(), F_GETFL, 0);
    if (flags < 0 ||
        ::fcntl(acceptor.native_handle(), F_SETFL, flags | O_NONBLOCK) != 0) {
      return false;
    }
    port = bound_loopback_endpoint(acceptor).port();
    return true;
  }
};

[[nodiscard]] std::array<int, 2> make_socketpair() {
  int sockets[2] = {-1, -1};
  const int rc = ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets);
  EXPECT_EQ(rc, 0);
  return {sockets[0], sockets[1]};
}

template <bool Eager>
void recv_ready_success() {
  bnio::io_context context(eager_options<Eager>());
  auto scheduler = context.get_post_scheduler();

  auto sockets = make_socketpair();
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);

  constexpr std::string_view payload = "eager-ready";
  EXPECT_EQ(::send(sender_socket.native_handle(), payload.data(),
                   payload.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(payload.size()));

  std::array<char, 32> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender =
      scheduler.async_read_some(receiver_socket.view(), bnio::buffer(bytes), 0);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

template <bool Eager>
void recv_eagain_then_ready() {
  bnio::io_context context(eager_options<Eager>());
  auto scheduler = context.get_post_scheduler();

  auto sockets = make_socketpair();
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);

  std::array<char, 32> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  // Start with no data pending: the first attempt returns EAGAIN and both
  // modes park on readiness.
  auto sender =
      scheduler.async_read_some(receiver_socket.view(), bnio::buffer(bytes), 0);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  constexpr std::string_view payload = "eager-passive";
  EXPECT_EQ(::send(sender_socket.native_handle(), payload.data(),
                   payload.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(payload.size()));

  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

template <bool Eager>
void recv_eof() {
  bnio::io_context context(eager_options<Eager>());
  auto scheduler = context.get_post_scheduler();

  auto sockets = make_socketpair();
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);

  // Peer sends nothing and half-closes: recv must observe EOF (ec={}, size 0).
  EXPECT_EQ(::shutdown(sender_socket.native_handle(), SHUT_WR), 0);

  std::array<char, 16> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender =
      scheduler.async_read_some(receiver_socket.view(), bnio::buffer(bytes), 0);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, 0u);
}

template <bool Eager>
void send_ready_success() {
  bnio::io_context context(eager_options<Eager>());
  auto scheduler = context.get_post_scheduler();

  auto sockets = make_socketpair();
  bnio::tcp_socket sender_socket(sockets[0]);
  bnio::tcp_socket receiver_socket(sockets[1]);

  constexpr std::string_view payload = "eager-send";
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = scheduler.async_write_some(sender_socket.view(),
                                           bnio::buffer(payload), MSG_NOSIGNAL);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());

  std::array<char, 32> bytes{};
  EXPECT_EQ(
      ::recv(receiver_socket.native_handle(), bytes.data(), bytes.size(), 0),
      static_cast<ssize_t>(payload.size()));
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

template <bool Eager>
void send_eagain_then_ready() {
  bnio::io_context context(eager_options<Eager>());
  auto scheduler = context.get_post_scheduler();

  auto sockets = make_socketpair();
  bnio::tcp_socket sender_socket(sockets[0]);
  bnio::tcp_socket receiver_socket(sockets[1]);

  const int sender_flags = ::fcntl(sender_socket.native_handle(), F_GETFL, 0);
  EXPECT_TRUE(sender_flags >= 0);
  EXPECT_EQ(::fcntl(sender_socket.native_handle(), F_SETFL,
                    sender_flags | O_NONBLOCK),
            0);

  // Fill the send buffer so the write_some attempt must wait for EVFILT_WRITE
  // / a drained peer instead of completing immediately.
  std::array<char, 4096> filler{};
  while (true) {
    const ssize_t result = ::send(sender_socket.native_handle(), filler.data(),
                                  filler.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (result > 0) {
      continue;
    }
    EXPECT_LT(result, 0);
    if (errno == EINTR) {
      continue;
    }
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    break;
  }

  constexpr std::string_view payload = "eager fallback";
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;
  auto sender = scheduler.async_write_some(sender_socket.view(),
                                           bnio::buffer(payload), MSG_NOSIGNAL);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  // Drain the peer before run() so the pending write can complete.
  std::array<char, 65536> drain{};
  while (true) {
    const ssize_t result = ::recv(receiver_socket.native_handle(), drain.data(),
                                  drain.size(), MSG_DONTWAIT);
    if (result > 0) {
      continue;
    }
    EXPECT_LT(result, 0);
    if (errno == EINTR) {
      continue;
    }
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    break;
  }

  context.run();

  EXPECT_EQ(state->signal, signal_kind::value);
  EXPECT_EQ(state->size, payload.size());
  std::array<char, 32> bytes{};
  EXPECT_EQ(
      ::recv(receiver_socket.native_handle(), bytes.data(), bytes.size(), 0),
      static_cast<ssize_t>(payload.size()));
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

template <bool Eager>
void accept_ready_success() {
  bnio::io_context context(eager_options<Eager>());
  auto scheduler = context.get_post_scheduler();

  loopback_listener listener;
  ASSERT_TRUE(listener.setup());

  // Pre-connect a real client so a pending connection exists before start().
  const int client_fd = connect_loopback_ready(listener.port);
  ASSERT_GE(client_fd, 0);

  fd_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = scheduler.async_accept(listener.acceptor.view(),
                                       SOCK_CLOEXEC | SOCK_NONBLOCK);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  ASSERT_EQ(state->signal, signal_kind::value);
  EXPECT_GE(state->fd, 0);
  EXPECT_TRUE((::fcntl(state->fd, F_GETFL, 0) & O_NONBLOCK) != 0);
  EXPECT_EQ(::close(state->fd), 0);
  state->fd = -1;
  EXPECT_EQ(::close(client_fd), 0);
}

template <bool Eager>
void accept_eagain_then_ready() {
  bnio::io_context context(eager_options<Eager>());
  auto scheduler = context.get_post_scheduler();

  loopback_listener listener;
  ASSERT_TRUE(listener.setup());

  fd_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  // Start the accept first: no connection is pending, so the eager probe
  // returns EAGAIN and both modes park on readiness.
  auto sender = scheduler.async_accept(listener.acceptor.view(),
                                       SOCK_CLOEXEC | SOCK_NONBLOCK);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  // Trigger the pending connection before run() drains the context.
  const int client_fd = connect_loopback_ready(listener.port);
  ASSERT_GE(client_fd, 0);

  context.run();

  ASSERT_EQ(state->signal, signal_kind::value);
  EXPECT_GE(state->fd, 0);
  EXPECT_EQ(::close(state->fd), 0);
  state->fd = -1;
  EXPECT_EQ(::close(client_fd), 0);
}

template <bool Eager>
void connect_ready_success() {
  bnio::io_context context(eager_options<Eager>());
  auto scheduler = context.get_post_scheduler();

  loopback_listener listener;
  ASSERT_TRUE(listener.setup());

  bnio::tcp_socket client;
  ASSERT_FALSE(client.open(bnio::ip::tcp::v4()));

  void_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = scheduler.async_connect(
      client.view(), bnio::ip::endpoint::loopback_v4(listener.port));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  ASSERT_EQ(state->signal, signal_kind::value);
  EXPECT_FALSE(state->error);
  // No fake success: with ec empty the socket must really be connected.
  EXPECT_TRUE(client.is_open());
  sockaddr_storage peer{};
  socklen_t peer_size = sizeof(peer);
  EXPECT_EQ(::getpeername(client.native_handle(),
                          reinterpret_cast<sockaddr*>(&peer), &peer_size),
            0);
}

template <bool Eager>
void connect_refused_or_reasonable_error() {
  bnio::io_context context(eager_options<Eager>());
  auto scheduler = context.get_post_scheduler();

  // Reserve an ephemeral loopback port, then close the probe so no TCP
  // listener exists there: the connect must not spuriously succeed.
  std::uint16_t port = 0;
  {
    bnio::tcp_acceptor probe;
    ASSERT_FALSE(probe.open(bnio::ip::tcp::v4()));
    ASSERT_FALSE(probe.bind(bnio::ip::endpoint::loopback_v4(0)));
    port = bound_loopback_endpoint(probe).port();
  }

  bnio::tcp_socket client;
  ASSERT_FALSE(client.open(bnio::ip::tcp::v4()));

  void_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = scheduler.async_connect(client.view(),
                                        bnio::ip::endpoint::loopback_v4(port));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  ASSERT_NE(state->signal, signal_kind::stopped);
  if (state->signal == signal_kind::value) {
    // No error: the connection must really be established.
    sockaddr_storage peer{};
    socklen_t peer_size = sizeof(peer);
    EXPECT_EQ(::getpeername(client.native_handle(),
                            reinterpret_cast<sockaddr*>(&peer), &peer_size),
              0);
  } else {
    ASSERT_EQ(state->signal, signal_kind::error);
    const bool reasonable =
        state->error == std::make_error_code(std::errc::connection_refused) ||
        state->error == std::make_error_code(std::errc::connection_reset) ||
        state->error == std::make_error_code(std::errc::timed_out) ||
        state->error == std::make_error_code(std::errc::network_unreachable) ||
        state->error == std::make_error_code(std::errc::host_unreachable) ||
        state->error == std::make_error_code(std::errc::address_not_available);
    EXPECT_TRUE(reasonable)
        << "unexpected connect ec: " << state->error.message();
  }
}

template <bool Eager>
void file_write_read() {
  std::string path = "/tmp/bnio-eager-file-XXXXXX";
  const int fd = ::mkstemp(path.data());
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::unlink(path.c_str()), 0);

  constexpr std::string_view payload = "eager optional file io";

  {
    bnio::io_context context(eager_options<Eager>());
    if (!context_available(context)) {
      EXPECT_EQ(::close(fd), 0);
      return;
    }
    auto scheduler = context.get_post_scheduler();

    byte_receiver receiver;
    receiver.context = &context;
    auto state = receiver.state;

    auto sender = scheduler.async_write_some(
        bnio::async_io::descriptor_view(fd), bnio::buffer(payload), 0);
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    context.run();

    ASSERT_EQ(state->signal, signal_kind::value);
    EXPECT_EQ(state->size, payload.size());
  }

  {
    bnio::io_context context(eager_options<Eager>());
    if (!context_available(context)) {
      EXPECT_EQ(::close(fd), 0);
      return;
    }
    auto scheduler = context.get_post_scheduler();
    std::array<char, 64> bytes{};

    byte_receiver receiver;
    receiver.context = &context;
    auto state = receiver.state;

    auto sender = scheduler.async_read_some(bnio::async_io::descriptor_view(fd),
                                            bnio::buffer(bytes), 0);
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    context.run();

    ASSERT_EQ(state->signal, signal_kind::value);
    EXPECT_EQ(state->size, payload.size());
    EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
  }

  EXPECT_EQ(::close(fd), 0);
}

template <bool Eager>
void invalid_descriptor_errors() {
  {
    bnio::io_context context(eager_options<Eager>());
    auto scheduler = context.get_post_scheduler();
    std::array<char, 8> bytes{};
    byte_receiver receiver;
    receiver.context = &context;
    auto state = receiver.state;

    auto sender = scheduler.async_read_some(bnio::async_io::descriptor_view(),
                                            bnio::buffer(bytes), 0);
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    context.run();

    EXPECT_EQ(state->signal, signal_kind::error);
    EXPECT_EQ(state->error, std::error_code(EBADF, std::generic_category()));
  }

  {
    bnio::io_context context(eager_options<Eager>());
    auto scheduler = context.get_post_scheduler();
    constexpr std::string_view payload = "invalid descriptor";
    byte_receiver receiver;
    receiver.context = &context;
    auto state = receiver.state;

    auto sender = scheduler.async_write_some(bnio::async_io::descriptor_view(),
                                             bnio::buffer(payload), 0);
    auto operation = bexec::connect(std::move(sender), std::move(receiver));
    bexec::start(operation);
    context.run();

    EXPECT_EQ(state->signal, signal_kind::error);
    EXPECT_EQ(state->error, std::error_code(EBADF, std::generic_category()));
  }
}

template <bool Eager>
void pre_stopped_token_stops() {
  bnio::io_context context(eager_options<Eager>());
  auto scheduler = context.get_post_scheduler();

  auto sockets = make_socketpair();
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);

  bexec::inplace_stop_source source;
  EXPECT_TRUE(source.request_stop());

  std::array<char, 8> bytes{};
  stopped_byte_receiver receiver;
  receiver.context = &context;
  receiver.env = stop_env{source.get_token()};
  auto state = receiver.state;

  auto sender =
      scheduler.async_read_some(receiver_socket.view(), bnio::buffer(bytes), 0);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  // Contract: a stop token already canceled at start() is observed by the
  // operation and completes via set_stopped (not
  // set_value(operation_canceled)).
  EXPECT_EQ(state->signal, signal_kind::stopped);
}

template <bool Eager>
void io_context_stop_aborts_inflight_read() {
  bnio::io_context context(eager_options<Eager>());
  auto scheduler = context.get_post_scheduler();

  auto sockets = make_socketpair();
  bnio::tcp_socket receiver_socket(sockets[0]);
  bnio::tcp_socket sender_socket(sockets[1]);  // peer never writes

  std::array<char, 16> bytes{};
  pair_byte_receiver receiver;
  // context=nullptr, completions=nullptr: receiver does NOT self-stop; the
  // main thread calls context.stop() to interrupt the inflight read.
  receiver.context = nullptr;
  receiver.completions = nullptr;
  auto state = receiver.state;

  auto sender =
      scheduler.async_read_some(receiver_socket.view(), bnio::buffer(bytes), 0);
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  // Synchronization: post a schedule task that runs once the worker enters
  // the run loop, confirming the worker is active and parked on the read.
  std::atomic<bool> worker_active{false};
  struct active_receiver {
    std::atomic<bool>* flag;
    void set_value(std::error_code) noexcept {
      flag->store(true, std::memory_order_release);
    }
    void set_stopped() noexcept {
      flag->store(true, std::memory_order_release);
    }
  };
  auto schedule_sender = context.get_post_scheduler().schedule();
  auto schedule_op =
      bexec::connect(schedule_sender, active_receiver{&worker_active});
  bexec::start(schedule_op);

  std::thread worker([&context] { context.run(); });

  {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!worker_active.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        (void)context.stop();
        worker.join();
        FAIL() << "Timed out waiting for worker to become active";
      }
      std::this_thread::yield();
    }
    // Margin: let the worker publish the read (register EVFILT_READ / submit
    // the SQE) before stop() interrupts it.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_GE(context.stop(), 0);
  worker.join();

  // Contract: context stop aborting inflight I/O completes via
  // set_value(operation_canceled), not set_stopped.
  EXPECT_EQ(state->signal, signal_kind::error);
  EXPECT_EQ(state->error, std::make_error_code(std::errc::operation_canceled));
}

template <bool Eager>
void udp_send_receive() {
  bnio::io_context context(eager_options<Eager>());
  auto scheduler = context.get_post_scheduler();

  bnio::udp::socket server;
  bnio::udp::socket client;
  ASSERT_FALSE(server.open(bnio::ip::udp::v4()));
  ASSERT_FALSE(client.open(bnio::ip::udp::v4()));
  ASSERT_FALSE(server.bind(bnio::ip::endpoint::loopback_v4(0)));
  ASSERT_FALSE(client.bind(bnio::ip::endpoint::loopback_v4(0)));

  bnio::ip::endpoint server_endpoint;
  bnio::ip::endpoint client_endpoint;
  ASSERT_FALSE(server.local_endpoint(server_endpoint));
  ASSERT_FALSE(client.local_endpoint(client_endpoint));
  ASSERT_FALSE(server.connect(client_endpoint));
  ASSERT_FALSE(client.connect(server_endpoint));

  constexpr std::string_view payload = "eager-udp";
  std::array<char, 32> bytes{};
  unsigned completions = 0;

  pair_byte_receiver receive_receiver;
  receive_receiver.context = &context;
  receive_receiver.completions = &completions;
  receive_receiver.target = 2;
  auto receive_state = receive_receiver.state;

  pair_byte_receiver send_receiver;
  send_receiver.context = &context;
  send_receiver.completions = &completions;
  send_receiver.target = 2;
  auto send_state = send_receiver.state;

  auto receive_sender =
      scheduler.async_receive(server.view(), bnio::buffer(bytes), 0);
  auto send_sender =
      scheduler.async_send(client.view(), bnio::buffer(payload), 0);

  auto receive_operation =
      bexec::connect(std::move(receive_sender), std::move(receive_receiver));
  auto send_operation =
      bexec::connect(std::move(send_sender), std::move(send_receiver));

  bexec::start(receive_operation);
  bexec::start(send_operation);
  context.run();

  EXPECT_EQ(completions, 2u);
  EXPECT_EQ(receive_state->signal, signal_kind::value);
  EXPECT_EQ(receive_state->size, payload.size());
  EXPECT_EQ(send_state->signal, signal_kind::value);
  EXPECT_EQ(send_state->size, payload.size());
  EXPECT_TRUE(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

}  // namespace

TEST(IoContextEagerOptionalTest, recv_immediate_success_eager_on_and_off) {
  bnio::io_context probe;
  if (!context_available(probe)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  recv_ready_success<true>();
  recv_ready_success<false>();
}

TEST(IoContextEagerOptionalTest, recv_eagain_then_ready_eager_on_and_off) {
  bnio::io_context probe;
  if (!context_available(probe)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  recv_eagain_then_ready<true>();
  recv_eagain_then_ready<false>();
}

TEST(IoContextEagerOptionalTest, recv_eof_eager_on_and_off) {
  bnio::io_context probe;
  if (!context_available(probe)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  recv_eof<true>();
  recv_eof<false>();
}

TEST(IoContextEagerOptionalTest, send_immediate_success_eager_on_and_off) {
  bnio::io_context probe;
  if (!context_available(probe)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  send_ready_success<true>();
  send_ready_success<false>();
}

TEST(IoContextEagerOptionalTest, send_eagain_then_ready_eager_on_and_off) {
  bnio::io_context probe;
  if (!context_available(probe)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  send_eagain_then_ready<true>();
  send_eagain_then_ready<false>();
}

TEST(IoContextEagerOptionalTest, accept_immediate_success_eager_on_and_off) {
  bnio::io_context probe;
  if (!context_available(probe)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  accept_ready_success<true>();
  accept_ready_success<false>();
}

TEST(IoContextEagerOptionalTest, accept_eagain_then_ready_eager_on_and_off) {
  bnio::io_context probe;
  if (!context_available(probe)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  accept_eagain_then_ready<true>();
  accept_eagain_then_ready<false>();
}

TEST(IoContextEagerOptionalTest, connect_ready_success_eager_on_and_off) {
  bnio::io_context probe;
  if (!context_available(probe)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  connect_ready_success<true>();
  connect_ready_success<false>();
}

TEST(IoContextEagerOptionalTest, connect_no_listener_eager_on_and_off) {
  bnio::io_context probe;
  if (!context_available(probe)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  connect_refused_or_reasonable_error<true>();
  connect_refused_or_reasonable_error<false>();
}

TEST(IoContextEagerOptionalTest, file_write_read_eager_on_and_off) {
  bnio::io_context probe;
  if (!context_available(probe)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  file_write_read<true>();
  file_write_read<false>();
}

TEST(IoContextEagerOptionalTest, invalid_descriptor_eager_on_and_off) {
  bnio::io_context probe;
  if (!context_available(probe)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  invalid_descriptor_errors<true>();
  invalid_descriptor_errors<false>();
}

TEST(IoContextEagerOptionalTest, pre_stopped_token_stops_eager_on_and_off) {
  bnio::io_context probe;
  if (!context_available(probe)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  pre_stopped_token_stops<true>();
  pre_stopped_token_stops<false>();
}

TEST(IoContextEagerOptionalTest,
     io_context_stop_aborts_inflight_eager_on_and_off) {
  bnio::io_context probe;
  if (!context_available(probe)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  io_context_stop_aborts_inflight_read<true>();
  io_context_stop_aborts_inflight_read<false>();
}

TEST(IoContextEagerOptionalTest, udp_send_receive_eager_on_and_off) {
  bnio::io_context probe;
  if (!context_available(probe)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  udp_send_receive<true>();
  udp_send_receive<false>();
}
