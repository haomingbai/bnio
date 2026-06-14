#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using asio::ip::tcp;

constexpr std::size_t k_default_message_size = 1024;
constexpr int k_default_connections = 128;
constexpr int k_default_duration_sec = 10;

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

int main(int argc, char** argv) {
  const auto port = static_cast<std::uint16_t>(parse_arg(argv, argc, 1, 8090));
  const auto connections =
      static_cast<int>(parse_arg(argv, argc, 2, k_default_connections));
  const auto duration_sec =
      static_cast<int>(parse_arg(argv, argc, 3, k_default_duration_sec));
  const auto message_size = static_cast<std::size_t>(
      parse_arg(argv, argc, 4, k_default_message_size));

  asio::io_context ctx;
  std::atomic<std::uint64_t> total{0};
  std::vector<char> send_buf(message_size, 'x');

  for (int i = 0; i < connections; ++i) {
    tcp::socket sk(ctx);
    sk.connect(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), port));

    struct conn : std::enable_shared_from_this<conn> {
      tcp::socket sk;
      std::vector<char> sbuf;
      std::vector<char> rbuf;
      std::atomic<std::uint64_t>& total;

      conn(tcp::socket s, const std::vector<char>& b,
           std::atomic<std::uint64_t>& t)
          : sk(std::move(s)), sbuf(b), rbuf(b.size()), total(t) {}

      void go() { send(); }

      void send() {
        auto self = shared_from_this();
        asio::async_write(sk, asio::buffer(sbuf),
                          [self](std::error_code ec, std::size_t) {
                            if (ec) {
                              return;
                            }
                            self->recv();
                          });
      }

      void recv() {
        auto self = shared_from_this();
        asio::async_read(sk, asio::buffer(rbuf),
                         [self](std::error_code ec, std::size_t) {
                           if (ec) {
                             return;
                           }
                           self->total++;
                           self->send();
                         });
      }
    };

    std::make_shared<conn>(std::move(sk), send_buf, total)->go();
  }

  std::thread timer([&] {
    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
    ctx.stop();
  });

  const auto start = std::chrono::steady_clock::now();
  ctx.run();
  const auto end = std::chrono::steady_clock::now();
  timer.join();

  const double secs = std::chrono::duration<double>(end - start).count();
  const std::uint64_t n = total.load();
  const double rate = static_cast<double>(n) / secs;
  const double throughput = static_cast<double>(n) *
                            static_cast<double>(message_size) / secs / 1024.0 /
                            1024.0;
  std::cout << "connections: " << connections << "\n"
            << "message_size: " << message_size << " bytes\n"
            << "duration: " << secs << " s\n"
            << "total: " << n << " echoes\n"
            << "rate: " << static_cast<std::uint64_t>(rate) << " req/s\n"
            << "throughput: " << static_cast<std::uint64_t>(throughput)
            << " MB/s\n";
}
