#include <bupp/bupp.h>
#include <sys/socket.h>
#include <bexec/bexec.hpp>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace bupp;
namespace {

constexpr std::uint16_t k_port = 8090;
constexpr int k_backlog = 512;

// Simple heap-allocated operation holder.
// The HTTP echo server uses a full spawn() with retire tracking;
// for a benchmark we just keep ops alive until shutdown.
std::vector<std::unique_ptr<io_context::operation_base>> g_ops;

template<class S, class R>
void spawn(S&& snd, R&& recv) {
  using op_t = decltype(bexec::connect(std::declval<S>(), std::declval<R>()));
  struct holder : io_context::operation_base {
    op_t op;
    holder(S&& s, R&& r) : op(bexec::connect(std::forward<S>(s), std::forward<R>(r))) {}
    void prepare(base::submission_queue_entry&) noexcept override {}
    int prepare_for_submit() noexcept override { return 0; }
    void complete_submit_error(int) noexcept override {}
    void execute() noexcept override {}
    void start() noexcept { bexec::start(op); }
  };
  auto h = std::make_unique<holder>(std::forward<S>(snd), std::forward<R>(recv));
  h->start();
  g_ops.push_back(std::move(h));
}

struct conn : std::enable_shared_from_this<conn> {
  io_context& ctx;
  tcp_socket  sk;
  std::string buf;

  conn(io_context& c, tcp_socket s) : ctx(c), sk(std::move(s)) { buf.reserve(4096); }
  void go() { recv(); }

  void recv() {
    buf.clear();
    struct R { std::shared_ptr<conn> c;
      void set_value(std::size_t m) noexcept { if(m){c->send(m);}else c->sk.close(); }
      void set_error(std::error_code) noexcept { c->sk.close(); }
      void set_stopped() noexcept { c->sk.close(); }
    };
    spawn(ctx.async_receive(sk, dynamic_buffer(buf), 0), R{shared_from_this()});
  }

  void send(std::size_t n) {
    struct R { std::shared_ptr<conn> c;
      void set_value(std::size_t) noexcept { c->recv(); }
      void set_error(std::error_code) noexcept { c->sk.close(); }
      void set_stopped() noexcept { c->sk.close(); }
    };
    spawn(ctx.async_write(sk, const_buffer(buf.data(), n)), R{shared_from_this()});
  }
};

void do_accept(io_context& ctx, tcp_acceptor& a) {
  struct R { io_context& ctx; tcp_acceptor& a;
    void set_value(tcp_socket sk) noexcept { std::make_shared<conn>(ctx,std::move(sk))->go(); do_accept(ctx,a); }
    void set_error(std::error_code) noexcept {}
    void set_stopped() noexcept {}
  };
  spawn(ctx.async_accept(a, SOCK_CLOEXEC), R{ctx,a});
}

} // namespace

int main() {
  io_context_options opts;
  opts.platform.uring.entries = 1024;
  opts.platform.uring.setup_flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN;
  io_context ctx(opts);
  if (!ctx.is_open()) { std::cerr << "ctx unavailable\n"; return 1; }

  tcp_acceptor a;
  a.open(ip::tcp::v4()); a.set_reuse_address(true);
  a.bind(ip::endpoint::loopback_v4(k_port)); a.listen(k_backlog);

  do_accept(ctx, a);

  std::cout << "bupp_raw_echo " << k_port << std::endl;
  ctx.run();
}
