// Timer cancellation — a 100ms timer cancels a pending 10s timer.
// The canceled wait completes with operation_canceled; the receiver
// observes that error and stops the context for a clean, immediate exit.
// An op_registry keeps every operation state alive until its completion
// has been delivered (the operations are non-movable).

#include <bnio/bnio.h>

#include <bexec/bexec.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct op_base {
  virtual ~op_base() = default;
};

class op_registry {
 public:
  template <class Sender, class Receiver>
  void spawn(Sender&& s, Receiver&& r) {
    using op_type = decltype(bexec::connect(std::declval<Sender>(),
                                            std::declval<Receiver>()));
    struct holder final : op_base {
      op_type op;
      holder(Sender&& ss, Receiver&& rr)
          : op(bexec::connect(std::forward<Sender>(ss),
                              std::forward<Receiver>(rr))) {}
    };
    auto h = std::make_unique<holder>(std::forward<Sender>(s),
                                      std::forward<Receiver>(r));
    bexec::start(h->op);
    ops_.push_back(std::move(h));
  }
  void clear() noexcept { ops_.clear(); }

 private:
  std::vector<std::unique_ptr<op_base>> ops_;
};

struct state : std::enable_shared_from_this<state> {
  bnio::io_context& ctx;
  op_registry reg;
  bnio::steady_timer long_t, short_t;

  explicit state(bnio::io_context& c)
      : ctx(c),
        long_t(c, std::chrono::seconds(10)),
        short_t(c, std::chrono::milliseconds(100)) {}
};

}  // namespace

int main() {
  bnio::io_context ctx;
  if (!ctx.is_open()) {
    std::cerr << "context unavailable\n";
    return 1;
  }

  auto s = std::make_shared<state>(ctx);

  struct RLong {
    std::shared_ptr<state> s;
    void set_value(std::error_code ec) noexcept {
      if (ec == std::make_error_code(std::errc::operation_canceled)) {
        std::cout << "10s timer canceled before expiry\n";
      } else if (ec) {
        std::cerr << "long wait: " << ec.message() << '\n';
      } else {
        std::cout << "10s timer expired (unexpected)\n";
      }
      s->ctx.stop();
    }
    void set_stopped() noexcept { s->ctx.stop(); }
  };
  s->reg.spawn(s->long_t.async_wait(), RLong{s});

  struct RShort {
    std::shared_ptr<state> s;
    void set_value(std::error_code ec) noexcept {
      if (ec) {
        std::cerr << "short wait: " << ec.message() << '\n';
        s->ctx.stop();
        return;
      }
      std::cout << "100ms up, canceling 10s timer\n";
      (void)s->long_t.cancel();
    }
    void set_stopped() noexcept { s->ctx.stop(); }
  };
  s->reg.spawn(s->short_t.async_wait(), RShort{s});

  std::cout << "armed 10s timer; canceling it after 100ms\n";
  ctx.run();
  return 0;
}
