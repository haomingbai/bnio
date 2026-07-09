#include <array>
#include <asio.hpp>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using asio::ip::tcp;

constexpr std::uint16_t k_port = 8091;
constexpr int k_backlog = 512;
constexpr std::size_t k_buf = 4096;
constexpr unsigned long k_default_workers = 1;
constexpr unsigned long k_max_workers = 1024;

[[nodiscard]] unsigned long parse_arg(char** argv, int argc, int index,
                                      unsigned long fallback) {
  if (argc <= index) {
    return fallback;
  }

  char* end = nullptr;
  const unsigned long value = std::strtoul(argv[index], &end, 10);
  if (end == argv[index] || *end != '\0' || value == 0) {
    return fallback;
  }
  return value;
}

[[nodiscard]] unsigned parse_workers(char** argv, int argc, int index) {
  const unsigned long value =
      parse_arg(argv, argc, index, k_default_workers);
  if (value > k_max_workers) {
    return static_cast<unsigned>(k_max_workers);
  }
  return static_cast<unsigned>(value);
}

asio::awaitable<void> echo_session(tcp::socket sk) {
  std::array<char, k_buf> buf{};

  while (true) {
    std::error_code ec;
    std::size_t n = co_await sk.async_read_some(
        asio::buffer(buf), asio::redirect_error(asio::use_awaitable, ec));
    if (ec || n == 0) {
      co_return;
    }

    std::size_t send_offset = 0;
    while (send_offset < n) {
      const std::size_t sent = co_await asio::async_write(
          sk, asio::buffer(buf.data() + send_offset, n - send_offset),
          asio::redirect_error(asio::use_awaitable, ec));
      if (ec || sent == 0) {
        co_return;
      }
      send_offset += sent;
    }
  }
}

asio::awaitable<void> accept_loop(tcp::acceptor& acceptor) {
  auto executor = co_await asio::this_coro::executor;
  while (true) {
    std::error_code ec;
    tcp::socket sk = co_await acceptor.async_accept(
        asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
      co_return;
    }
    asio::co_spawn(executor, echo_session(std::move(sk)), asio::detached);
  }
}

void run_context(asio::io_context& ctx, unsigned worker_count) {
  std::vector<std::thread> workers;
  workers.reserve(worker_count - 1);

  for (unsigned index = 1; index < worker_count; ++index) {
    workers.emplace_back([&ctx] { ctx.run(); });
  }

  ctx.run();

  for (std::thread& worker : workers) {
    worker.join();
  }
}

int main(int argc, char** argv) {
  const auto port =
      static_cast<std::uint16_t>(parse_arg(argv, argc, 1, k_port));
  const unsigned worker_count = parse_workers(argv, argc, 2);

  asio::io_context ctx;
  tcp::acceptor a(ctx);

  std::error_code ec;
  a.open(tcp::v4(), ec);
  if (ec) {
    std::cerr << "open failed: " << ec.message() << '\n';
    return 1;
  }
  a.set_option(asio::socket_base::reuse_address(true), ec);
  if (ec) {
    std::cerr << "setsockopt failed: " << ec.message() << '\n';
    return 1;
  }
  a.bind(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), port), ec);
  if (ec) {
    std::cerr << "bind failed: " << ec.message() << '\n';
    return 1;
  }
  a.listen(k_backlog, ec);
  if (ec) {
    std::cerr << "listen failed: " << ec.message() << '\n';
    return 1;
  }

  asio::signal_set sigs(ctx, SIGINT, SIGTERM);
  sigs.async_wait([&ctx](auto, int) { ctx.stop(); });

  asio::co_spawn(ctx, accept_loop(a), asio::detached);

  std::cout << "asio_raw_echo " << port << " workers " << worker_count
            << std::endl;
  run_context(ctx, worker_count);
}
