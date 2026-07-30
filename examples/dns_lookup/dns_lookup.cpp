#include <bnio/bnio.h>
#include <bexec/bexec.hpp>

#include <array>
#include <cstdio>
#include <string>
#include <system_error>

struct resolve_receiver {
  bnio::io_context* context;
  bnio::dns_result_view results;

  void set_value(std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
      const auto& ep = results[i];
      const auto& addr = ep.address();
      if (const auto* v4 = addr.v4()) {
        std::printf("%d.%d.%d.%d:%d\n",
                    (*v4)[0], (*v4)[1], (*v4)[2], (*v4)[3], ep.port());
      } else if (addr.is_v6()) {
        std::printf("[IPv6]:%d\n", ep.port());
      }
    }
    context->stop();
  }

  void set_error(std::error_code ec) noexcept {
    std::fprintf(stderr, "%s\n", ec.message().c_str());
    context->stop();
  }

  void set_stopped() noexcept { context->stop(); }
};

int main(int argc, char** argv) {
  std::string host = (argc >= 2) ? argv[1] : "localhost";
  std::string service = (argc >= 3) ? argv[2] : "80";

  bnio::io_context context;

  std::array<bnio::ip::endpoint, 8> results{};
  auto sender = context.get_post_scheduler().async_resolve(
      host, service, bnio::dns_result_view(results));

  auto op = bexec::connect(
      std::move(sender),
      resolve_receiver{&context, bnio::dns_result_view(results)});
  bexec::start(op);

  context.run();
  return 0;
}
