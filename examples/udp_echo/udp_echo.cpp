// UDP echo — send a datagram and receive the reply.
// main thread runs context.run(). An op_registry keeps every operation
// state alive until its completion has been delivered (the operations
// are non-movable).

#include <bnio/bnio.h>

#include <array>
#include <bexec/bexec.hpp>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kB = 2048;

// Minimal type-erased operation holder.
struct op_base {
  virtual ~op_base() = default;
};
class op_registry {
 public:
  template <class Sender, class Receiver>
  void spawn(Sender&& s, Receiver&& r) {
    using op_t = decltype(bexec::connect(std::declval<Sender>(),
                                         std::declval<Receiver>()));
    struct H final : op_base {
      op_t op;
      H(Sender&& ss, Receiver&& rr)
          : op(bexec::connect(std::forward<Sender>(ss),
                              std::forward<Receiver>(rr))) {}
    };
    auto h =
        std::make_unique<H>(std::forward<Sender>(s), std::forward<Receiver>(r));
    bexec::start(h->op);
    ops_.push_back(std::move(h));
  }
  void clear() noexcept { ops_.clear(); }

 private:
  std::vector<std::unique_ptr<op_base>> ops_;
};

struct state : std::enable_shared_from_this<state> {
  bnio::io_context& ctx;
  bnio::udp_socket so;
  op_registry reg;
  std::string msg;
  std::array<char, kB> buf{};
  bnio::ip::endpoint peer{};

  state(bnio::io_context& c, bnio::udp_socket s) : ctx(c), so(std::move(s)) {}

  void send(bnio::ip::endpoint to, std::string m) {
    msg = std::move(m);  // the send buffer must outlive the operation
    struct R {
      std::shared_ptr<state> s;
      void set_value(std::error_code ec, std::size_t) noexcept {
        if (ec) {
          std::cerr << ec.message() << '\n';
          s->done();
          return;
        }
        s->recv();
      }
      void set_stopped() noexcept { s->done(); }
    };
    reg.spawn(
        so.async_send_to(ctx.get_post_scheduler(),
                         bnio::const_buffer(msg.data(), msg.size()), to, 0),
        R{shared_from_this()});
  }

  void recv() {
    struct R {
      std::shared_ptr<state> s;
      void set_value(std::error_code ec, std::size_t n) noexcept {
        if (ec) {
          std::cerr << ec.message() << '\n';
          s->done();
          return;
        }
        std::cout.write(s->buf.data(), static_cast<std::streamsize>(n));
        std::cout << std::flush;
        s->done();
      }
      void set_stopped() noexcept { s->done(); }
    };
    reg.spawn(so.async_receive_from(ctx.get_post_scheduler(), bnio::buffer(buf),
                                    peer, 0),
              R{shared_from_this()});
  }

  void done() {
    (void)so.close();
    ctx.stop();
  }
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
  if (!ctx.is_open()) {
    std::cerr << "context unavailable\n";
    return 1;
  }

  bnio::udp_socket so;
  if (auto ec = so.open(bnio::ip::udp::v4())) {
    std::cerr << ec.message() << '\n';
    return 1;
  }

  auto s = std::make_shared<state>(ctx, std::move(so));
  s->send(bnio::ip::endpoint(bnio::ip::address::loopback_v4(),
                             static_cast<std::uint16_t>(std::stoi(port))),
          msg);
  ctx.run();
  return 0;
}
