// UDP echo — send a datagram and receive the reply.
// main thread runs context.run().

#include <bnio/bnio.h>
#include <bexec/bexec.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

namespace {

constexpr std::size_t kB = 2048;

struct state : std::enable_shared_from_this<state> {
  bnio::io_context& ctx;
  bnio::udp_socket so;
  std::array<char, kB> buf{};
  bnio::ip::endpoint peer{};

  state(bnio::io_context& c, bnio::udp_socket s) : ctx(c), so(std::move(s)) {}

  void send(bnio::ip::endpoint to, std::string msg) {
    struct R {
      std::shared_ptr<state> s;
      void set_value(std::size_t) noexcept { s->recv(); }
      void set_error(std::error_code e) noexcept {
        std::cerr << e.message() << '\n'; s->done();
      }
      void set_stopped() noexcept { s->done(); }
    };
    auto op = bexec::connect(
        so.async_send_to(ctx.get_post_scheduler(),
                         bnio::const_buffer(msg.data(), msg.size()), to, 0),
        R{shared_from_this()});
    bexec::start(op);
  }

  void recv() {
    struct R {
      std::shared_ptr<state> s;
      void set_value(std::size_t n) noexcept {
        std::cout.write(s->buf.data(), static_cast<std::streamsize>(n));
        std::cout << std::flush;
        s->done();
      }
      void set_error(std::error_code e) noexcept {
        std::cerr << e.message() << '\n'; s->done();
      }
      void set_stopped() noexcept { s->done(); }
    };
    auto op = bexec::connect(
        so.async_receive_from(ctx.get_post_scheduler(),
                              bnio::buffer(buf), peer, 0),
        R{shared_from_this()});
    bexec::start(op);
  }

  void done() { (void)so.close(); ctx.stop(); }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: " << argv[0] << " <host> <port> [msg]\n";
    return 2;
  }
  std::string host = argv[1], port = argv[2];
  std::string msg = (argc > 3) ? argv[3] : "hello";

  bnio::io_context ctx;
  if (!ctx.is_open()) { std::cerr << "context unavailable\n"; return 1; }

  bnio::udp_socket so;
  if (auto ec = so.open(bnio::ip::udp::v4())) {
    std::cerr << ec.message() << '\n'; return 1;
  }

  auto s = std::make_shared<state>(ctx, std::move(so));
  s->send(bnio::ip::endpoint(bnio::ip::address::loopback_v4(),
                             static_cast<std::uint16_t>(std::stoi(port))),
          msg);
  ctx.run();
  return 0;
}
