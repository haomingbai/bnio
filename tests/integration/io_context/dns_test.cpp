#include <bupp/io_context.h>
#include <gtest/gtest.h>

#include <array>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <system_error>
#include <utility>
#include <vector>

namespace {

enum class signal_kind {
  none,
  value,
  error,
  stopped,
};

struct resolve_state {
  signal_kind signal = signal_kind::none;
  std::size_t endpoint_count = 0;
  std::error_code error;
};

struct resolve_receiver {
  resolve_state* state = nullptr;
  bupp::io_context* context = nullptr;

  void set_value(std::size_t count) noexcept {
    if (state != nullptr) {
      state->signal = signal_kind::value;
      state->endpoint_count = count;
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    if (state != nullptr) {
      state->signal = signal_kind::error;
      state->error = error;
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    if (state != nullptr) {
      state->signal = signal_kind::stopped;
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

[[nodiscard]] bool context_available(const bupp::io_context& context) {
  return context.is_open();
}

TEST(DnsTest, sender_concepts) {
  bupp::io_context context;
  auto scheduler = context.get_post_scheduler();
  std::array<bupp::ip::endpoint, 4> results{};
  bupp::dns_result_view result_view(results);

  using query_sender = decltype(scheduler.async_resolve(
      bupp::dns_query("127.0.0.1", "80"), result_view));
  using string_sender =
      decltype(scheduler.async_resolve("127.0.0.1", "80", result_view));
  static_assert(bexec::sender<query_sender>);
  static_assert(bexec::sender<string_sender>);
  static_assert(
      bupp::resolves_dns<bupp::io_context::post_scheduler, bupp::dns_query>);

  auto sender =
      scheduler.async_resolve(bupp::dns_query("127.0.0.1", "80"), result_view);
  auto operation = bexec::connect(std::move(sender), resolve_receiver{});
  static_assert(bexec::operation_state<decltype(operation)>);
}

TEST(DnsTest, scheduler_resolves_numeric_address) {
  bupp::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  auto scheduler = context.get_post_scheduler();
  bupp::dns_query query("127.0.0.1", "8080");
  query.set_address_version(bupp::ip::address::version::v4);
  std::array<bupp::ip::endpoint, 8> results{};

  resolve_receiver receiver;
  resolve_state state;
  receiver.state = &state;
  receiver.context = &context;

  auto sender = bupp::async_resolve(scheduler, std::move(query),
                                    bupp::dns_result_view(results));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state.signal, signal_kind::value);
  EXPECT_GT(state.endpoint_count, 0);
  EXPECT_EQ(results[0].port(), 8080);
  EXPECT_EQ(results[0].address().type(), bupp::ip::address::version::v4);
}

TEST(DnsTest, scheduler_resolves_host_service_strings) {
  bupp::io_context context;
  if (!context_available(context)) {
    GTEST_SKIP() << "native I/O context is unavailable";
  }

  auto scheduler = context.get_post_scheduler();
  std::vector<bupp::ip::endpoint> results(8);
  resolve_receiver receiver;
  resolve_state state;
  receiver.state = &state;
  receiver.context = &context;

  auto sender = bupp::async_resolve(
      scheduler, "127.0.0.1", "8081",
      bupp::dns_result_view(results.data(), results.size()));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  EXPECT_EQ(state.signal, signal_kind::value);
  EXPECT_GT(state.endpoint_count, 0);
  EXPECT_EQ(results[0].port(), 8081);
}

}  // namespace
