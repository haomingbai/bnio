#include <array>
#include <asio.hpp>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <system_error>

using asio::ip::tcp;

constexpr std::uint16_t k_port = 8091;
constexpr int k_backlog = 512;
constexpr std::size_t k_buf = 4096;

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

int main(int argc, char** argv) {
  std::uint16_t port = k_port;
  if (argc > 1) {
    port = static_cast<std::uint16_t>(std::strtoul(argv[1], nullptr, 10));
  }

  asio::io_context ctx;
  tcp::acceptor a(ctx,
                  tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), port));
  a.listen(k_backlog);

  asio::signal_set sigs(ctx, SIGINT, SIGTERM);
  sigs.async_wait([&ctx](auto, int) { ctx.stop(); });

  asio::co_spawn(ctx, accept_loop(a), asio::detached);

  std::cout << "asio_raw_echo " << port << std::endl;
  ctx.run();
}
