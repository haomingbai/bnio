#include <bnio/io_context.h>
#include <bnio/ip.h>
#include <bnio/tcp.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

#include "../../support/io_context/io_context_loopback_test_support.h"

namespace {

struct tcp_stress_state {
  std::atomic<unsigned> completions{0};
  std::atomic<unsigned> errors{0};
  std::atomic<unsigned> stopped{0};
};

struct tcp_stress_receiver {
  tcp_stress_state* state;
  bnio::io_context* context;
  unsigned target_completions = 0;

  void set_value(std::error_code ec, std::size_t) noexcept { handle(ec); }
  void set_value(std::error_code ec) noexcept { handle(ec); }
  void set_value(std::error_code ec, int) noexcept { handle(ec); }
  void set_value(std::error_code ec, bnio::tcp_socket) noexcept { handle(ec); }
  void set_stopped() noexcept {
    // Triggered by stop-token cancellation; io_context::stop() aborts
    // in-flight work through set_value(operation_canceled)
    state->stopped.fetch_add(1, std::memory_order_relaxed);
    complete();
  }

 private:
  void handle(std::error_code ec) noexcept {
    if (ec) {
      state->errors.fetch_add(1, std::memory_order_relaxed);
    }
    complete();
  }
  void complete() noexcept {
    unsigned completed =
        state->completions.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (completed >= target_completions && context != nullptr) {
      (void)context->stop();
    }
  }
};

}  // namespace

TEST(TcpStressTest, many_concurrent_accept_connect) {
  bnio::io_context_options options;
  constexpr unsigned worker_count = 4;
  options.concurrency_hint = worker_count;
  bnio::io_context context(options);
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  bnio::tcp_acceptor acceptor;
  EXPECT_FALSE(acceptor.open(bnio::ip::tcp::v4()));
  EXPECT_FALSE(acceptor.set_reuse_address(true));
  EXPECT_FALSE(acceptor.bind(bnio::ip::endpoint::loopback_v4(0)));
  EXPECT_FALSE(acceptor.listen(200));
  const bnio::ip::endpoint endpoint = bound_loopback_endpoint(acceptor);

  constexpr int num_clients = 100;
  constexpr unsigned target = num_clients * 2;

  tcp_stress_state state;

  using accept_sender_type =
      decltype(acceptor.async_accept(scheduler, SOCK_CLOEXEC));
  using accept_operation_type = decltype(bexec::connect(
      std::declval<accept_sender_type>(), std::declval<tcp_stress_receiver>()));

  std::vector<std::unique_ptr<accept_operation_type>> accept_ops;
  accept_ops.resize(num_clients);

  for (size_t i = 0; i < num_clients; ++i) {
    accept_ops[i].reset(new accept_operation_type(
        bexec::connect(acceptor.async_accept(scheduler, SOCK_CLOEXEC),
                       tcp_stress_receiver{&state, &context, target})));
    bexec::start(*accept_ops[i]);
  }

  std::vector<bnio::tcp_socket> clients;
  clients.reserve(static_cast<std::size_t>(num_clients));

  using connect_sender_type =
      decltype(std::declval<bnio::tcp_socket&>().async_connect(scheduler,
                                                               endpoint));
  using connect_operation_type =
      decltype(bexec::connect(std::declval<connect_sender_type>(),
                              std::declval<tcp_stress_receiver>()));

  std::vector<std::unique_ptr<connect_operation_type>> connect_ops;
  connect_ops.resize(num_clients);

  for (size_t i = 0; i < num_clients; ++i) {
    clients.emplace_back();
    EXPECT_FALSE(clients.back().open(bnio::ip::tcp::v4()));
    connect_ops[i].reset(new connect_operation_type(
        bexec::connect(clients.back().async_connect(scheduler, endpoint),
                       tcp_stress_receiver{&state, &context, target})));
    bexec::start(*connect_ops[i]);
  }

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (unsigned i = 0; i < worker_count; ++i) {
    workers.emplace_back([&context] { context.run(); });
  }
  for (auto& w : workers) {
    w.join();
  }

  EXPECT_EQ(state.completions.load(std::memory_order_acquire), target);
  EXPECT_EQ(state.errors.load(std::memory_order_acquire), 0);
  EXPECT_EQ(state.stopped.load(std::memory_order_acquire), 0);
}

TEST(TcpStressTest, high_concurrency_read_write) {
  bnio::io_context_options options;
  constexpr unsigned worker_count = 4;
  options.concurrency_hint = worker_count;
  bnio::io_context context(options);
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  bnio::tcp_socket server_socket(sockets[0]);
  bnio::tcp_socket client_socket(sockets[1]);

  constexpr int num_ops = 1000;
  constexpr unsigned target = static_cast<unsigned>(num_ops * 2);

  tcp_stress_state state;
  std::vector<std::array<char, 10>> read_bufs(
      static_cast<std::size_t>(num_ops));
  std::vector<std::array<char, 10>> write_bufs(
      static_cast<std::size_t>(num_ops));
  for (int i = 0; i < num_ops; ++i) {
    write_bufs[static_cast<std::size_t>(i)].fill('A');
  }

  using read_sender_type =
      decltype(server_socket.async_read(scheduler, bnio::buffer(read_bufs[0])));
  using write_sender_type = decltype(client_socket.async_write(
      scheduler, bnio::buffer(write_bufs[0])));

  using read_op_type = decltype(bexec::connect(
      std::declval<read_sender_type>(), std::declval<tcp_stress_receiver>()));
  using write_op_type = decltype(bexec::connect(
      std::declval<write_sender_type>(), std::declval<tcp_stress_receiver>()));

  std::vector<std::unique_ptr<read_op_type>> read_ops;
  read_ops.resize(static_cast<std::size_t>(num_ops));
  std::vector<std::unique_ptr<write_op_type>> write_ops;
  write_ops.resize(static_cast<std::size_t>(num_ops));

  for (int i = 0; i < num_ops; ++i) {
    std::size_t idx = static_cast<std::size_t>(i);
    read_ops[idx].reset(new read_op_type(bexec::connect(
        server_socket.async_read(scheduler, bnio::buffer(read_bufs[idx])),
        tcp_stress_receiver{&state, &context, target})));
    bexec::start(*read_ops[idx]);

    write_ops[idx].reset(new write_op_type(bexec::connect(
        client_socket.async_write(scheduler, bnio::buffer(write_bufs[idx])),
        tcp_stress_receiver{&state, &context, target})));
    bexec::start(*write_ops[idx]);
  }

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (unsigned i = 0; i < worker_count; ++i) {
    workers.emplace_back([&context] { context.run(); });
  }
  for (auto& w : workers) {
    w.join();
  }

  EXPECT_EQ(state.completions.load(std::memory_order_acquire), target);
  EXPECT_EQ(state.errors.load(std::memory_order_acquire), 0);
  EXPECT_EQ(state.stopped.load(std::memory_order_acquire), 0);

  for (int i = 0; i < num_ops; ++i) {
    std::size_t idx = static_cast<std::size_t>(i);
    EXPECT_EQ(std::memcmp(read_bufs[idx].data(), "AAAAAAAAAA", 10), 0);
  }
}

TEST(TcpStressTest, rapid_connect_disconnect) {
  unsigned total_errors = 0;
  constexpr int num_iterations = 50;

  for (int i = 0; i < num_iterations; ++i) {
    bnio::io_context context;
    if (!context_available(context)) {
      GTEST_SKIP() << "native I/O context is unavailable";
    }
    auto scheduler = context.get_post_scheduler();

    bnio::tcp_acceptor acceptor;
    EXPECT_FALSE(acceptor.open(bnio::ip::tcp::v4()));
    EXPECT_FALSE(acceptor.set_reuse_address(true));
    EXPECT_FALSE(acceptor.bind(bnio::ip::endpoint::loopback_v4(0)));
    EXPECT_FALSE(acceptor.listen(4));
    const bnio::ip::endpoint endpoint = bound_loopback_endpoint(acceptor);

    bnio::tcp_socket server_socket;
    bnio::tcp_socket client_socket;
    EXPECT_FALSE(client_socket.open(bnio::ip::tcp::v4()));

    unsigned completions = 0;

    using accept_sender_type =
        decltype(acceptor.async_accept(scheduler, SOCK_CLOEXEC));
    using connect_sender_type =
        decltype(client_socket.async_connect(scheduler, endpoint));

    struct pair_recv {
      unsigned* completions;
      bnio::io_context* context;
      bnio::tcp_socket* server_storage;

      void set_value(std::error_code ec, bnio::tcp_socket socket) noexcept {
        if (ec) {
          ++(*completions);
          (void)context->stop();
          return;
        }
        *server_storage = std::move(socket);
        check_done();
      }
      void set_value(std::error_code ec) noexcept {
        if (ec) {
          ++(*completions);
          (void)context->stop();
          return;
        }
        check_done();
      }
      void set_stopped() noexcept { (void)context->stop(); }

     private:
      void check_done() noexcept {
        ++(*completions);
        if (*completions == 2) {
          (void)context->stop();
        }
      }
    };

    using accept_op_type = decltype(bexec::connect(
        std::declval<accept_sender_type>(), std::declval<pair_recv>()));
    using connect_op_type = decltype(bexec::connect(
        std::declval<connect_sender_type>(), std::declval<pair_recv>()));

    auto accept_op = accept_op_type(
        bexec::connect(acceptor.async_accept(scheduler, SOCK_CLOEXEC),
                       pair_recv{&completions, &context, &server_socket}));
    auto connect_op = connect_op_type(
        bexec::connect(client_socket.async_connect(scheduler, endpoint),
                       pair_recv{&completions, &context, nullptr}));

    bexec::start(accept_op);
    bexec::start(connect_op);
    context.run();

    if (completions != 2) {
      ++total_errors;
    }
  }

  EXPECT_EQ(total_errors, 0);
}
