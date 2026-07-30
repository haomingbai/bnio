// Chained timer — three timers in sequence (200ms → 100ms → 50ms).
// Each receiver spawns the next via an operation_registry that constructs
// the operation state in-place (the operations are non-movable).

#include <bnio/bnio.h>

#include <bexec/bexec.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <system_error>
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

struct chain : std::enable_shared_from_this<chain> {
  bnio::io_context& ctx;
  op_registry reg;
  bnio::steady_timer t1, t2, t3;

  chain(bnio::io_context& c)
      : ctx(c),
        t1(c, std::chrono::milliseconds(200)),
        t2(c, std::chrono::milliseconds(100)),
        t3(c, std::chrono::milliseconds(50)) {}

  void step2() {
    struct R {
      std::shared_ptr<chain> c;
      void set_value() noexcept { c->step3(); }
      void set_error(std::error_code) noexcept { c->ctx.stop(); }
      void set_stopped() noexcept { c->ctx.stop(); }
    };
    reg.spawn(t2.async_wait(), R{shared_from_this()});
  }

  void step3() {
    struct R {
      std::shared_ptr<chain> c;
      void set_value() noexcept {
        std::cout << "chain complete\n";
        c->ctx.stop();
      }
      void set_error(std::error_code) noexcept { c->ctx.stop(); }
      void set_stopped() noexcept { c->ctx.stop(); }
    };
    reg.spawn(t3.async_wait(), R{shared_from_this()});
  }
};

}  // namespace

int main() {
  bnio::io_context ctx;
  if (!ctx.is_open()) {
    std::cerr << "context unavailable\n";
    return 1;
  }

  auto c = std::make_shared<chain>(ctx);

  struct R {
    std::shared_ptr<chain> c;
    void set_value() noexcept { c->step2(); }
    void set_error(std::error_code) noexcept { c->ctx.stop(); }
    void set_stopped() noexcept { c->ctx.stop(); }
  };
  c->reg.spawn(c->t1.async_wait(), R{c});

  std::cout << "200ms → 100ms → 50ms\n";
  ctx.run();
  return 0;
}
