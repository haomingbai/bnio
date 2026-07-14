#include <bupp/async_io/ip/address.h>
#include <bupp/async_io/ip/endpoint.h>
#include <bupp/io_context.h>
#include <bupp/ip.h>
#include <bupp/udp.h>

#include <array>
#include <atomic>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "io_context_runtime_test_support.h"

namespace {

constexpr std::size_t datagram_count = 128;
constexpr std::size_t datagram_size = sizeof(std::uint32_t);

struct stress_state {
  std::atomic<unsigned> completions{0};
  std::atomic<unsigned> errors{0};
  std::atomic<unsigned> stopped{0};
  std::array<std::size_t, datagram_count> receive_sizes{};
  std::array<std::size_t, datagram_count> send_sizes{};
};

struct stress_receiver {
  stress_state* state;
  bupp::io_context* context;
  std::size_t index;
  bool receive;

  void set_value(std::size_t size) noexcept {
    if (receive) {
      state->receive_sizes[index] = size;
    } else {
      state->send_sizes[index] = size;
    }
    complete();
  }

  void set_error(std::error_code) noexcept {
    state->errors.fetch_add(1, std::memory_order_relaxed);
    complete();
  }

  void set_stopped() noexcept {
    state->stopped.fetch_add(1, std::memory_order_relaxed);
    complete();
  }

 private:
  void complete() noexcept {
    const unsigned completed =
        state->completions.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (completed == datagram_count * 2) {
      (void)context->stop();
    }
  }
};

void run_stress_test() {
  bupp::io_context_options options;
  constexpr unsigned worker_count = 4;
  options.concurrency_hint = worker_count;
#if defined(BUPP_SYSTEM_LINUX)
  options.platform.uring.entries = 512;
#else
  options.platform.kqueue.entries = 512;
#endif
  bupp::io_context context(options);
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  bupp::udp::socket receiver_socket;
  bupp::udp::socket sender_socket;
  assert(!receiver_socket.open(bupp::ip::udp::v4()));
  assert(!sender_socket.open(bupp::ip::udp::v4()));
  assert(!receiver_socket.bind(bupp::ip::endpoint::loopback_v4(0)));
  assert(!sender_socket.bind(bupp::ip::endpoint::loopback_v4(0)));

  bupp::ip::endpoint receiver_endpoint;
  bupp::ip::endpoint sender_endpoint;
  assert(!receiver_socket.local_endpoint(receiver_endpoint));
  assert(!sender_socket.local_endpoint(sender_endpoint));

  using bytes = std::array<char, datagram_size>;
  std::array<bytes, datagram_count> sent{};
  std::array<bytes, datagram_count> received{};
  std::array<bupp::ip::endpoint, datagram_count> sources{};
  for (std::size_t index = 0; index < datagram_count; ++index) {
    const auto value = static_cast<std::uint32_t>(index);
    std::memcpy(sent[index].data(), &value, sizeof(value));
  }

  auto make_receive_sender = [&](std::size_t index) {
    return receiver_socket.async_receive_from(scheduler, received[index],
                                              sources[index]);
  };
  auto make_send_sender = [&](std::size_t index) {
    return sender_socket.async_send_to(scheduler, sent[index],
                                       receiver_endpoint);
  };

  using receive_sender_type = decltype(make_receive_sender(0));
  using send_sender_type = decltype(make_send_sender(0));
  using receive_operation_type = decltype(bexec::connect(
      std::declval<receive_sender_type>(), std::declval<stress_receiver>()));
  using send_operation_type = decltype(bexec::connect(
      std::declval<send_sender_type>(), std::declval<stress_receiver>()));

  stress_state state;
  std::array<std::unique_ptr<receive_operation_type>, datagram_count>
      receive_operations;
  std::array<std::unique_ptr<send_operation_type>, datagram_count>
      send_operations;

  for (std::size_t index = 0; index < datagram_count; ++index) {
    receive_operations[index].reset(new receive_operation_type(
        bexec::connect(make_receive_sender(index),
                       stress_receiver{&state, &context, index, true})));
    bexec::start(*receive_operations[index]);
  }
  for (std::size_t index = 0; index < datagram_count; ++index) {
    send_operations[index].reset(new send_operation_type(
        bexec::connect(make_send_sender(index),
                       stress_receiver{&state, &context, index, false})));
    bexec::start(*send_operations[index]);
  }

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (unsigned index = 0; index < worker_count; ++index) {
    workers.emplace_back([&context] { context.run(); });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  assert(state.completions.load(std::memory_order_acquire) ==
         datagram_count * 2);
  assert(state.errors.load(std::memory_order_acquire) == 0);
  assert(state.stopped.load(std::memory_order_acquire) == 0);

  std::array<bool, datagram_count> observed{};
  for (std::size_t index = 0; index < datagram_count; ++index) {
    assert(state.receive_sizes[index] == datagram_size);
    assert(state.send_sizes[index] == datagram_size);
    assert(sources[index].address().is_v4());
    assert(sources[index].address().to_v4() ==
           sender_endpoint.address().to_v4());
    assert(sources[index].port() == sender_endpoint.port());

    std::uint32_t value = 0;
    std::memcpy(&value, received[index].data(), sizeof(value));
    assert(value < datagram_count);
    assert(!observed[value]);
    observed[value] = true;
  }
  for (bool value : observed) {
    assert(value);
  }
}

}  // namespace

int main() {
  run_stress_test();
  return 0;
}
