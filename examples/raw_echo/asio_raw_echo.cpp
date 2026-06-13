#include <asio.hpp>
#include <array>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>

using asio::ip::tcp;

constexpr std::uint16_t k_port = 8091;
constexpr int k_backlog = 512;
constexpr std::size_t k_buf = 4096;

struct session : std::enable_shared_from_this<session> {
  tcp::socket sk;
  std::array<char,k_buf> buf{};
  std::size_t n = 0;

  explicit session(tcp::socket s) : sk(std::move(s)) {}
  void go() { recv(); }

  void recv() {
    auto self = shared_from_this();
    sk.async_read_some(asio::buffer(buf), [self](std::error_code ec, std::size_t m) {
      if (ec || !m) return;
      self->n = m; self->send();
    });
  }

  void send() {
    auto self = shared_from_this();
    asio::async_write(self->sk, asio::buffer(self->buf.data(), self->n),
      [self](std::error_code ec, std::size_t) {
        if (ec) return;
        self->recv();
      });
  }
};

int main() {
  asio::io_context ctx;
  tcp::acceptor a(ctx, tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), k_port));
  a.listen(k_backlog);

  asio::signal_set sigs(ctx, SIGINT, SIGTERM);
  sigs.async_wait([&ctx](auto, int) { ctx.stop(); });

  auto do_accept = [&](auto&& self) -> void {
    a.async_accept([&,self](std::error_code ec, tcp::socket sk) {
      if (!ec) std::make_shared<session>(std::move(sk))->go();
      self(self);
    });
  };
  do_accept(do_accept);

  std::cout << "asio_raw_echo " << k_port << std::endl;
  ctx.run();
}
