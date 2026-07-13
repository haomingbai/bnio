#include <bupp/async_io/ip/address.h>
#include <bupp/async_io/ip/endpoint.h>
#include <bupp/async_io/linux/io_uring_context_base/options.h>
#include <bupp/ip.h>
#include <bupp/linux/detail/io_context_options.h>
#include <bupp/linux/io_context.h>
#include <bupp/udp.h>

#include <array>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <system_error>
#include <utility>

#include "io_context_runtime_test_support.h"

namespace {

constexpr std::size_t datagram_count = 128;
constexpr std::size_t datagram_size = sizeof(std::uint32_t);

struct stress_state {
  unsigned completions = 0;
  unsigned errors = 0;
  unsigned stopped = 0;
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
    ++state->errors;
    complete();
  }

  void set_stopped() noexcept {
    ++state->stopped;
    complete();
  }

 private:
  void complete() noexcept {
    if (++state->completions == datagram_count * 2) {
      (void)context->stop();
    }
  }
};

template <bool Direct>
void run_stress_test() {
  bupp::io_context_options options;
  options.platform.uring.entries = 512;
  options.platform.max_queued_io_operations = 512;
  options.platform.queued_io_flush_after = std::chrono::seconds(30);
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
    if constexpr (Direct) {
      return receiver_socket.async_receive_from_direct(
          scheduler, received[index], sources[index]);
    } else {
      return receiver_socket.async_receive_from(scheduler, received[index],
                                                sources[index]);
    }
  };
  auto make_send_sender = [&](std::size_t index) {
    if constexpr (Direct) {
      return sender_socket.async_send_to_direct(scheduler, sent[index],
                                                receiver_endpoint);
    } else {
      return sender_socket.async_send_to(scheduler, sent[index],
                                         receiver_endpoint);
    }
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

  if constexpr (Direct) {
    assert(scheduler.queued_io_size() == 0);
  } else {
    assert(scheduler.queued_io_size() == datagram_count);
    assert(!scheduler.flush_io_queue());
  }
  context.run();

  assert(state.completions == datagram_count * 2);
  assert(state.errors == 0);
  assert(state.stopped == 0);

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
  run_stress_test<false>();
  run_stress_test<true>();
  return 0;
}
