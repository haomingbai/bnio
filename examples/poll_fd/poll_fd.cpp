#include <bnio/bnio.h>
#include <bexec/bexec.hpp>

#include <array>
#include <iostream>
#include <system_error>

struct poll_receiver {
  bnio::io_context* ctx = nullptr;

  void set_value(unsigned events) noexcept {
    if (events & POLLIN) {
      std::array<char, 1024> buf{};
      std::cin.getline(buf.data(), buf.size());
      if (std::cin.gcount() > 0)
        std::cout << "echo: " << buf.data() << '\n';
    }
    ctx->stop();
  }

  void set_error(std::error_code ec) noexcept {
    std::cerr << "poll failed: " << ec.message() << '\n';
    ctx->stop();
  }

  void set_stopped() noexcept { ctx->stop(); }
};

int main() {
  bnio::io_context ctx;
  if (!ctx.is_open()) {
    std::cerr << "io_context unavailable\n";
    return 1;
  }

  std::cerr << "Type something and press Enter...\n";

  auto sender = ctx.get_post_scheduler().async_poll(
      bnio::async_io::descriptor_view(STDIN_FILENO), POLLIN);

  auto op = bexec::connect(std::move(sender), poll_receiver{&ctx});
  bexec::start(op);

  ctx.run();
  return 0;
}
