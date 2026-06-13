#include <asio.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using asio::ip::tcp;

constexpr std::size_t k_buf = 1024;
constexpr int k_connections = 128;
constexpr int k_duration_sec = 10;

int main(int argc, char** argv) {
  std::uint16_t port = 8090;
  if (argc > 1) port = static_cast<std::uint16_t>(std::strtoul(argv[1], nullptr, 10));

  asio::io_context ctx;
  std::atomic<std::uint64_t> total{0};
  std::atomic<int> done{0};

  // Fill buffer with non-zero data
  std::array<char, k_buf> send_buf{};
  for (auto& c : send_buf) c = 'x';

  for (int i = 0; i < k_connections; ++i) {
    tcp::socket sk(ctx);
    sk.connect(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), port));

    struct conn : std::enable_shared_from_this<conn> {
      tcp::socket sk;
      std::array<char,k_buf> sbuf;
      std::array<char,k_buf> rbuf{};
      std::size_t rn = 0;
      std::atomic<std::uint64_t>& total;
      std::atomic<int>& done;

      conn(tcp::socket s, const std::array<char,k_buf>& b,
           std::atomic<std::uint64_t>& t, std::atomic<int>& d)
          : sk(std::move(s)), sbuf(b), total(t), done(d) {}

      void go() { send(); }

      void send() {
        auto self = shared_from_this();
        asio::async_write(sk, asio::buffer(sbuf),
          [self](std::error_code ec, std::size_t) {
            if (ec) { self->done++; return; }
            self->recv();
          });
      }

      void recv() {
        auto self = shared_from_this();
        rn = 0;
        asio::async_read(sk, asio::buffer(rbuf, k_buf),
          [self](std::error_code ec, std::size_t) {
            if (ec) { self->done++; return; }
            self->total++;
            self->send();
          });
      }
    };

    std::make_shared<conn>(std::move(sk), send_buf, total, done)->go();
  }

  // Run for fixed duration
  std::thread timer([&] {
    std::this_thread::sleep_for(std::chrono::seconds(k_duration_sec));
    ctx.stop();
  });

  auto start = std::chrono::steady_clock::now();
  ctx.run();
  auto end = std::chrono::steady_clock::now();
  timer.join();

  double secs = std::chrono::duration<double>(end - start).count();
  std::uint64_t n = total.load();
  std::cout << "connections: " << k_connections << "\n"
            << "duration: " << secs << " s\n"
            << "total: " << n << " echoes\n"
            << "rate: " << static_cast<std::uint64_t>(n / secs) << " req/s\n"
            << "throughput: " << static_cast<std::uint64_t>(n * k_buf / secs / 1024 / 1024) << " MB/s\n";
}
