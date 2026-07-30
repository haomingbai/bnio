// TCP client with timeout — resolve → connect → send → receive.
// main thread runs context.run(). steady_timer aborts on timeout (10s).

#include <bnio/bnio.h>
#include <bexec/bexec.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace {

constexpr std::size_t kN = 8, kB = 4096;

struct client : std::enable_shared_from_this<client> {
  bnio::io_context& ctx;
  bnio::tcp_socket so;
  std::string msg;
  std::array<bnio::ip::endpoint, kN> ep{};
  std::array<char, kB> buf{};
  std::unique_ptr<bnio::steady_timer> tm;

  client(bnio::io_context& c, bnio::tcp_socket s, std::string m)
      : ctx(c), so(std::move(s)), msg(std::move(m)) {}

  void arm_timeout() {
    tm = std::make_unique<bnio::steady_timer>(ctx, std::chrono::seconds(10));
    struct R {
      std::shared_ptr<client> c;
      void set_value() noexcept { c->fail("timeout"); }
      void set_error(std::error_code) noexcept {}
      void set_stopped() noexcept {}
    };
    auto op = bexec::connect(tm->async_wait(), R{shared_from_this()});
    bexec::start(op);
  }

  void cancel_timeout() { if (tm) { (void)tm->cancel(); tm.reset(); } }

  void connect(std::size_t i) {
    if (i >= kN) { fail("no endpoints"); return; }
    const auto& e = ep[i];
    std::error_code ec;
    if (e.version() == bnio::ip::address::version::v4)
      ec = so.open(bnio::ip::tcp::v4());
    else
      ec = so.open(bnio::ip::tcp::v6());
    if (ec) { connect(i + 1); return; }

    struct R {
      std::shared_ptr<client> c;
      void set_value() noexcept { c->send(); }
      void set_error(std::error_code) noexcept { c->fail("connect failed"); }
      void set_stopped() noexcept { c->done(); }
    };
    auto op = bexec::connect(so.async_connect(ctx.get_post_scheduler(), e),
                             R{shared_from_this()});
    bexec::start(op);
  }

  void send() {
    struct R {
      std::shared_ptr<client> c;
      void set_value(std::size_t) noexcept { c->recv(); }
      void set_error(std::error_code e) noexcept { c->fail("send", e); }
      void set_stopped() noexcept { c->done(); }
    };
    auto op = bexec::connect(
        so.async_write(ctx.get_post_scheduler(),
                       bnio::const_buffer(msg.data(), msg.size()),
                       MSG_NOSIGNAL),
        R{shared_from_this()});
    bexec::start(op);
  }

  void recv() {
    struct R {
      std::shared_ptr<client> c;
      void set_value(std::size_t n) noexcept {
        if (n > 0) {
          std::cout.write(c->buf.data(), static_cast<std::streamsize>(n));
          c->recv();
        } else {
          c->done();
        }
      }
      void set_error(std::error_code e) noexcept {
        if (e == std::make_error_code(std::errc::connection_reset))
          c->done();
        else
          c->fail("read", e);
      }
      void set_stopped() noexcept { c->done(); }
    };
    auto op = bexec::connect(
        so.async_read(ctx.get_post_scheduler(), bnio::buffer(buf)),
        R{shared_from_this()});
    bexec::start(op);
  }

  void fail(std::string_view s, std::error_code e = {}) {
    std::cerr << s;
    if (e) std::cerr << ": " << e.message();
    std::cerr << '\n';
    done();
  }

  void done() {
    if (stopped_.exchange(true)) return;
    cancel_timeout();
    (void)so.close();
    ctx.stop();
  }

 private:
  std::atomic<bool> stopped_{false};
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: " << argv[0] << " <host> <port> [msg]\n";
    return 2;
  }
  std::string host = argv[1], port = argv[2];
  std::string msg = (argc > 3) ? std::string(argv[3]) + "\n" : "hello\n";

  bnio::io_context ctx;
  if (!ctx.is_open()) { std::cerr << "context unavailable\n"; return 1; }

  bnio::dns_query q(host, port);
  q.set_transport(bnio::dns_transport::tcp);
  std::array<bnio::ip::endpoint, kN> ep{};

  struct R {
    bnio::io_context* ctx;
    std::string msg;
    bnio::dns_result_view res;

    void set_value(std::size_t n) noexcept {
      if (n == 0) { std::cerr << "no endpoints\n"; ctx->stop(); return; }
      bnio::tcp_socket so;
      auto c = std::make_shared<client>(*ctx, std::move(so), std::move(msg));
      for (std::size_t i = 0; i < n; ++i) c->ep[i] = res[i];
      c->arm_timeout();
      c->connect(0);
    }
    void set_error(std::error_code e) noexcept {
      std::cerr << "resolve: " << e.message() << '\n'; ctx->stop();
    }
    void set_stopped() noexcept { ctx->stop(); }
  };

  auto s = ctx.get_post_scheduler().async_resolve(
      std::move(q), bnio::dns_result_view(ep));
  auto op = bexec::connect(std::move(s),
                           R{&ctx, msg, bnio::dns_result_view(ep)});
  bexec::start(op);
  ctx.run();
  return 0;
}
