// TCP echo server — structured concurrency with sender/receiver.
// main thread runs context.run(). SIGINT triggers graceful shutdown.
// Each session uses an operation_registry to keep async ops alive.

#include <bnio/bnio.h>

#include <array>
#include <atomic>
#include <bexec/bexec.hpp>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t k_buf = 4096;

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

class echo_server;
class echo_session;

// ------------------------------------------------------------------
// echo_session
// ------------------------------------------------------------------
class echo_session : public std::enable_shared_from_this<echo_session> {
 public:
  echo_session(bnio::io_context& c, bnio::tcp_socket so,
               std::shared_ptr<echo_server> s);
  void start();

 private:
  void do_read();
  void do_write();
  void done();

  bnio::io_context& ctx_;
  bnio::tcp_socket so_;
  std::weak_ptr<echo_server> srv_;
  op_registry reg_;
  std::array<char, k_buf> buf_{};
  std::size_t nw_ = 0;
};

// ------------------------------------------------------------------
// echo_server
// ------------------------------------------------------------------
class echo_server : public std::enable_shared_from_this<echo_server> {
 public:
  echo_server(bnio::io_context& c, bnio::tcp_acceptor a)
      : ctx_(c), acp_(std::move(a)) {}

  void start();
  void shutdown() {
    if (stopped_.exchange(true)) return;
    stopping_ = true;
    (void)acp_.close();
    if (n_ == 0) ctx_.stop();
  }
  void on_begin() noexcept { ++n_; }
  void on_end() noexcept {
    if (--n_ == 0 && stopping_ && !stopped_.exchange(true)) ctx_.stop();
  }

 private:
  void do_accept();
  bnio::io_context& ctx_;
  bnio::tcp_acceptor acp_;
  op_registry reg_;
  unsigned n_ = 0;
  bool stopping_ = false;
  std::atomic<bool> stopped_{false};
};

std::weak_ptr<echo_server> g_srv{};
void on_signal(int) {
  if (auto s = g_srv.lock()) s->shutdown();
}

// --- echo_session impl ---

echo_session::echo_session(bnio::io_context& c, bnio::tcp_socket so,
                           std::shared_ptr<echo_server> s)
    : ctx_(c), so_(std::move(so)), srv_(std::move(s)) {}

void echo_session::start() { do_read(); }

void echo_session::do_read() {
  struct R {
    std::shared_ptr<echo_session> se;
    void set_value(std::error_code ec, std::size_t n) noexcept {
      if (ec || n == 0) {
        se->done();
        return;
      }
      se->nw_ = n;
      se->do_write();
    }
    void set_stopped() noexcept { se->done(); }
  };
  reg_.spawn(so_.async_read(ctx_.get_post_scheduler(), bnio::buffer(buf_)),
             R{shared_from_this()});
}

void echo_session::do_write() {
  struct R {
    std::shared_ptr<echo_session> se;
    void set_value(std::error_code ec, std::size_t) noexcept {
      if (ec) {
        se->done();
        return;
      }
      se->do_read();
    }
    void set_stopped() noexcept { se->done(); }
  };
  reg_.spawn(
      so_.async_write(ctx_.get_post_scheduler(),
                      bnio::const_buffer(buf_.data(), nw_), MSG_NOSIGNAL),
      R{shared_from_this()});
}

void echo_session::done() {
  (void)so_.close();
  if (auto s = srv_.lock()) s->on_end();
}

// --- echo_server impl ---

void echo_server::start() { do_accept(); }

void echo_server::do_accept() {
  if (stopping_) return;
  struct R {
    std::shared_ptr<echo_server> s;
    void set_value(std::error_code ec, bnio::tcp_socket so) noexcept {
      if (ec) return;
      auto se = std::make_shared<echo_session>(s->ctx_, std::move(so), s);
      s->on_begin();
      se->start();
      s->do_accept();
    }
    void set_stopped() noexcept {}
  };
  reg_.spawn(acp_.async_accept(ctx_.get_post_scheduler(), 0),
             R{shared_from_this()});
}

std::uint16_t to_port(const std::string& s) {
  return static_cast<std::uint16_t>(std::stoi(s));
}

}  // namespace

int main(int argc, char** argv) {
  const std::string port = (argc > 1) ? argv[1] : "8080";
  bnio::io_context ctx;
  if (!ctx.is_open()) {
    std::cerr << "context unavailable\n";
    return 1;
  }

  bnio::tcp_acceptor acp;
  std::error_code ec;
  if ((ec = acp.open(bnio::ip::tcp::v4())) ||
      (ec = acp.bind(
           bnio::ip::endpoint(bnio::ip::address::any_v4(), to_port(port)))) ||
      (ec = acp.listen(128))) {
    std::cerr << ec.message() << '\n';
    return 1;
  }

  std::cerr << "listening on 0.0.0.0:" << port << " (ctrl-c to stop)\n";
  auto srv = std::make_shared<echo_server>(ctx, std::move(acp));
  g_srv = srv;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  srv->start();
  ctx.run();
  std::cerr << "shutdown complete.\n";
  return 0;
}
