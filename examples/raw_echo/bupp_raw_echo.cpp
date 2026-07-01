#include <bupp/bupp.h>
#include <sys/socket.h>

#include <array>
#include <bexec/bexec.hpp>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

using namespace bupp;
namespace {

constexpr std::uint16_t k_port = 8090;
constexpr int k_backlog = 512;
constexpr std::size_t k_buffer_size = 4096;

// Simple heap-allocated operation holder.
// For a benchmark we keep ops alive until shutdown.
std::vector<std::unique_ptr<io_context::operation_base>> g_ops;

template <class S, class R>
void spawn(S&& snd, R&& recv) {
  using op_t = decltype(bexec::connect(std::declval<S>(), std::declval<R>()));
  struct holder : io_context::operation_base {
    op_t op;
    holder(S&& s, R&& r)
        : op(bexec::connect(std::forward<S>(s), std::forward<R>(r))) {}
    void prepare(base::submission_queue_entry&) noexcept override {}
    int prepare_for_submit() noexcept override { return 0; }
    void complete_submit_error(int) noexcept override {}
    void execute() noexcept override {}
    void start() noexcept { bexec::start(op); }
  };
  auto h =
      std::make_unique<holder>(std::forward<S>(snd), std::forward<R>(recv));
  h->start();
  g_ops.push_back(std::move(h));
}

struct conn : std::enable_shared_from_this<conn> {
  io_context& ctx;
  tcp_socket sk;
  std::array<char, k_buffer_size> buf{};
  std::size_t send_offset = 0;
  std::size_t send_size = 0;

  conn(io_context& c, tcp_socket s) : ctx(c), sk(std::move(s)) {}
  void go() { recv(); }
  void close() noexcept { (void)sk.close(); }

  void recv() {
    struct R {
      std::shared_ptr<conn> c;
      void set_value(std::size_t m) noexcept {
        if (m) {
          c->send(m);
        } else
          c->close();
      }
      void set_error(std::error_code) noexcept { c->close(); }
      void set_stopped() noexcept { c->close(); }
    };
    auto scheduler = ctx.get_post_scheduler();
    spawn(sk.async_receive(scheduler, buffer(buf), 0), R{shared_from_this()});
  }

  void send(std::size_t n) {
    send_offset = 0;
    send_size = n;
    send_next();
  }

  void send_next() {
    struct R {
      std::shared_ptr<conn> c;
      void set_value(std::size_t n) noexcept {
        if (n == 0) {
          c->close();
          return;
        }
        c->send_offset += n;
        if (c->send_offset < c->send_size) {
          c->send_next();
        } else {
          c->recv();
        }
      }
      void set_error(std::error_code) noexcept { c->close(); }
      void set_stopped() noexcept { c->close(); }
    };
    auto scheduler = ctx.get_post_scheduler();
    spawn(sk.async_send(
              scheduler,
              const_buffer(buf.data() + send_offset, send_size - send_offset),
              MSG_NOSIGNAL),
          R{shared_from_this()});
  }
};

void do_accept(io_context& ctx, tcp_acceptor& a) {
  struct R {
    io_context& ctx;
    tcp_acceptor& a;
    void set_value(tcp_socket sk) noexcept {
      std::make_shared<conn>(ctx, std::move(sk))->go();
      do_accept(ctx, a);
    }
    void set_error(std::error_code) noexcept {}
    void set_stopped() noexcept {}
  };
  auto scheduler = ctx.get_post_scheduler();
  spawn(a.async_accept(scheduler, SOCK_CLOEXEC), R{ctx, a});
}

}  // namespace

int main(int argc, char** argv) {
  std::uint16_t port = k_port;
  if (argc > 1) {
    port = static_cast<std::uint16_t>(std::strtoul(argv[1], nullptr, 10));
  }

  io_context_options opts;
  opts.platform.uring.entries = 1024;
  opts.platform.uring.setup_flags = IORING_SETUP_COOP_TASKRUN;
  io_context ctx(opts);
  if (!ctx.is_open()) {
    std::cerr << "ctx unavailable\n";
    return 1;
  }

  tcp_acceptor a;
  if (const std::error_code ec = a.open(ip::tcp::v4())) {
    std::cerr << "open failed: " << ec.message() << '\n';
    return 1;
  }
  if (const std::error_code ec = a.set_reuse_address(true)) {
    std::cerr << "setsockopt failed: " << ec.message() << '\n';
    return 1;
  }
  if (const std::error_code ec = a.bind(ip::endpoint::loopback_v4(port))) {
    std::cerr << "bind failed: " << ec.message() << '\n';
    return 1;
  }
  if (const std::error_code ec = a.listen(k_backlog)) {
    std::cerr << "listen failed: " << ec.message() << '\n';
    return 1;
  }

  do_accept(ctx, a);

  std::cout << "bupp_raw_echo " << port << std::endl;
  ctx.run();
}
