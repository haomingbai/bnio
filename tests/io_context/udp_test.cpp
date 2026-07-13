#include <bupp/async_io/dns/resolve.h>
#include <bupp/async_io/dns/types.h>
#include <bupp/async_io/ip/address.h>
#include <bupp/async_io/ip/endpoint.h>
#include <bupp/async_io/socket_view.h>
#include <bupp/buffer/basic.h>
#include <bupp/buffer/dynamic_string.h>
#include <bupp/buffer/holders.h>
#include <bupp/ip.h>
#include <bupp/linux/detail/io_context_options.h>
#include <bupp/linux/io_context.h>
#include <bupp/udp.h>
#include <unistd.h>

#include <array>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "io_context_runtime_test_support.h"

namespace {

template <class Scheduler, class Socket, class Buffer>
concept scheduler_can_send_datagram =
    requires(Scheduler scheduler, Socket socket, Buffer buffer) {
      scheduler.async_send(socket, buffer);
    };

template <class Scheduler, class Socket, class Buffer>
concept scheduler_can_receive_datagram =
    requires(Scheduler scheduler, Socket socket, Buffer buffer) {
      scheduler.async_receive(socket, buffer);
    };

template <class Socket>
concept has_sync_datagram_io = requires(
    Socket socket, bupp::const_buffer output, bupp::mutable_buffer input,
    bupp::ip::endpoint endpoint, std::error_code error) {
  socket.send(output, 0, error);
  socket.receive(input, 0, error);
  socket.send_to(output, endpoint, 0, error);
  socket.receive_from(input, endpoint, 0, error);
};

struct pair_state {
  unsigned completions = 0;
  std::size_t sent = 0;
  std::size_t received = 0;
  std::error_code error;
};

struct send_receiver {
  pair_state* state;
  bupp::io_context* context;

  void set_value(std::size_t size) noexcept {
    state->sent = size;
    complete();
  }
  void set_error(std::error_code error) noexcept {
    state->error = error;
    complete();
  }
  void set_stopped() noexcept { complete(); }

 private:
  void complete() noexcept {
    if (++state->completions == 2) {
      (void)context->stop();
    }
  }
};

struct receive_receiver {
  pair_state* state;
  bupp::io_context* context;

  void set_value(std::size_t size) noexcept {
    state->received = size;
    complete();
  }
  void set_error(std::error_code error) noexcept {
    state->error = error;
    complete();
  }
  void set_stopped() noexcept { complete(); }

 private:
  void complete() noexcept {
    if (++state->completions == 2) {
      (void)context->stop();
    }
  }
};

void test_sender_concepts() {
  bupp::io_context context;
  auto scheduler = context.get_post_scheduler();
  bupp::udp::socket socket;
  std::array<char, 8> bytes{};
  const std::string_view payload = "udp";
  bupp::ip::endpoint endpoint;

  using send_sender = decltype(socket.async_send(scheduler, payload));
  using send_direct_sender =
      decltype(socket.async_send_direct(scheduler, payload));
  using receive_sender = decltype(socket.async_receive(scheduler, bytes));
  using receive_direct_sender =
      decltype(socket.async_receive_direct(scheduler, bytes));
  using send_to_sender =
      decltype(socket.async_send_to(scheduler, payload, endpoint));
  using send_to_direct_sender =
      decltype(socket.async_send_to_direct(scheduler, payload, endpoint));
  using receive_from_sender =
      decltype(socket.async_receive_from(scheduler, bytes, endpoint));
  using receive_from_direct_sender =
      decltype(socket.async_receive_from_direct(scheduler, bytes, endpoint));
  using low_send_sender =
      decltype(scheduler.async_send(socket.view(), bupp::buffer(payload)));
  using low_receive_sender =
      decltype(scheduler.async_receive(socket.view(), bupp::buffer(bytes)));

  static_assert(bexec::sender<send_sender>);
  static_assert(bexec::sender<send_direct_sender>);
  static_assert(bexec::sender<receive_sender>);
  static_assert(bexec::sender<receive_direct_sender>);
  static_assert(bexec::sender<send_to_sender>);
  static_assert(bexec::sender<send_to_direct_sender>);
  static_assert(bexec::sender<receive_from_sender>);
  static_assert(bexec::sender<receive_from_direct_sender>);
  static_assert(bexec::sender<low_send_sender>);
  static_assert(bexec::sender<low_receive_sender>);
  static_assert(
      scheduler_can_send_datagram<decltype(scheduler), decltype(socket.view()),
                                  bupp::const_buffer>);
  static_assert(
      scheduler_can_receive_datagram<
          decltype(scheduler), decltype(socket.view()), bupp::mutable_buffer>);
  static_assert(!scheduler_can_send_datagram<decltype(scheduler),
                                             bupp::async_io::stream_socket_view,
                                             bupp::const_buffer>);
  static_assert(
      !scheduler_can_receive_datagram<decltype(scheduler),
                                      bupp::async_io::stream_socket_view,
                                      bupp::mutable_buffer>);
  static_assert(
      !scheduler_can_write_stream<decltype(scheduler),
                                  bupp::async_io::datagram_socket_view,
                                  bupp::const_buffer>);
  static_assert(!scheduler_can_read_stream<decltype(scheduler),
                                           bupp::async_io::datagram_socket_view,
                                           bupp::mutable_buffer>);
}

void test_protocol_and_lifecycle() {
  static_assert(std::is_same_v<bupp::ip::udp::socket, bupp::udp::socket>);
  static_assert(std::is_same_v<bupp::udp_socket, bupp::udp::socket>);
  static_assert(!std::is_copy_constructible_v<bupp::udp::socket>);
  static_assert(std::is_nothrow_move_constructible_v<bupp::udp::socket>);
  static_assert(!has_sync_datagram_io<bupp::udp::socket>);

  assert(bupp::ip::udp::v4().version() == bupp::ip::address::version::v4);
  assert(bupp::ip::udp::v6().version() == bupp::ip::address::version::v6);

  auto query =
      bupp::udp::make_resolve_query("127.0.0.1", 443, bupp::ip::udp::v4());
  assert(query.transport() == bupp::dns_transport::udp);
  assert(query.address_version() == bupp::ip::address::version::v4);
  assert(query.service() == "443");
  std::array<bupp::ip::endpoint, 4> resolved{};
  std::size_t resolved_count = 0;
  assert(!bupp::async_io::resolve_dns(query, bupp::dns_result_view(resolved),
                                      resolved_count));
  assert(resolved_count > 0);
  assert(resolved[0].address().is_v4());
  assert(resolved[0].port() == 443);

  bupp::udp::socket unspecified;
  const std::error_code error = unspecified.open(bupp::ip::udp());
  assert(error == std::error_code(EAFNOSUPPORT, std::generic_category()));

  bupp::udp::socket first;
  assert(!first.open(bupp::ip::udp::v4()));
  const int fd = first.native_handle();
  assert(fd >= 0);
  bupp::udp::socket second(std::move(first));
  assert(!first.is_open());
  assert(second.native_handle() == fd);
  assert(second.release() == fd);
  assert(::close(fd) == 0);
}

template <bool Direct>
void test_async_send_to_receive_from() {
  bupp::io_context_options options;
  options.platform.max_queued_io_operations = 16;
  options.platform.queued_io_flush_after = std::chrono::seconds(30);
  bupp::io_context context(options);
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  bupp::udp::socket server;
  bupp::udp::socket client;
  assert(!server.open(bupp::ip::udp::v4()));
  assert(!client.open(bupp::ip::udp::v4()));
  assert(!server.bind(bupp::ip::endpoint::loopback_v4(0)));
  bupp::ip::endpoint server_endpoint;
  assert(!server.local_endpoint(server_endpoint));

  constexpr std::string_view payload = "asynchronous datagram";
  std::string received;
  bupp::ip::endpoint source;
  pair_state state;

  auto receive_sender = [&] {
    if constexpr (Direct) {
      return server.async_receive_from_direct(
          scheduler, bupp::dynamic_buffer(received), source);
    } else {
      return server.async_receive_from(scheduler,
                                       bupp::dynamic_buffer(received), source);
    }
  }();
  auto send_sender = [&] {
    if constexpr (Direct) {
      return client.async_send_to_direct(scheduler, payload, server_endpoint);
    } else {
      return client.async_send_to(scheduler, payload, server_endpoint);
    }
  }();

  auto receive_operation = bexec::connect(std::move(receive_sender),
                                          receive_receiver{&state, &context});
  auto send_operation =
      bexec::connect(std::move(send_sender), send_receiver{&state, &context});
  bexec::start(receive_operation);
  bexec::start(send_operation);

  if constexpr (Direct) {
    assert(scheduler.queued_io_size() == 0);
  } else {
    assert(scheduler.queued_io_size() == 1);
    assert(!scheduler.flush_io_queue());
  }
  context.run();

  assert(!state.error);
  assert(state.completions == 2);
  assert(state.sent == payload.size());
  assert(state.received == payload.size());
  assert(received == payload);
  assert(source.address().is_v4());
  assert(source.address().to_v4() == bupp::ip::address::loopback_v4().to_v4());
  assert(source.port() != 0);
}

template <bool Direct>
void test_async_default_peer() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  bupp::udp::socket server;
  bupp::udp::socket client;
  assert(!server.open(bupp::ip::udp::v4()));
  assert(!client.open(bupp::ip::udp::v4()));
  assert(!server.bind(bupp::ip::endpoint::loopback_v4(0)));
  assert(!client.bind(bupp::ip::endpoint::loopback_v4(0)));

  bupp::ip::endpoint server_endpoint;
  bupp::ip::endpoint client_endpoint;
  assert(!server.local_endpoint(server_endpoint));
  assert(!client.local_endpoint(client_endpoint));
  assert(!server.connect(client_endpoint));
  assert(!client.connect(server_endpoint));

  constexpr std::string_view payload = "default-peer";
  std::string received;
  pair_state state;
  auto receive_sender = [&] {
    if constexpr (Direct) {
      return server.async_receive_direct(scheduler,
                                         bupp::dynamic_buffer(received));
    } else {
      return server.async_receive(scheduler, bupp::dynamic_buffer(received));
    }
  }();
  auto send_sender = [&] {
    if constexpr (Direct) {
      return client.async_send_direct(scheduler, payload);
    } else {
      return client.async_send(scheduler, payload);
    }
  }();
  auto receive_operation = bexec::connect(std::move(receive_sender),
                                          receive_receiver{&state, &context});
  auto send_operation =
      bexec::connect(std::move(send_sender), send_receiver{&state, &context});
  bexec::start(receive_operation);
  bexec::start(send_operation);

  if constexpr (Direct) {
    assert(scheduler.queued_io_size() == 0);
  } else {
    assert(scheduler.queued_io_size() == 1);
    assert(!scheduler.flush_io_queue());
  }
  context.run();

  assert(!state.error);
  assert(state.completions == 2);
  assert(state.sent == payload.size());
  assert(state.received == payload.size());
  assert(received == payload);
}

void test_async_ipv6_send_to() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  bupp::udp::socket server;
  bupp::udp::socket client;
  const std::error_code server_open = server.open(bupp::ip::udp::v6());
  const std::error_code client_open = client.open(bupp::ip::udp::v6());
  if (server_open == std::errc::address_family_not_supported ||
      client_open == std::errc::address_family_not_supported) {
    return;
  }
  assert(!server_open);
  assert(!client_open);
  const std::error_code bind_error =
      server.bind(bupp::ip::endpoint::loopback_v6(0));
  if (bind_error == std::errc::address_not_available) {
    return;
  }
  assert(!bind_error);

  bupp::ip::endpoint destination;
  assert(!server.local_endpoint(destination));
  constexpr std::string_view payload = "ipv6";
  std::string received;
  bupp::ip::endpoint source;
  pair_state state;
  auto receive_operation =
      bexec::connect(server.async_receive_from(
                         scheduler, bupp::dynamic_buffer(received), source),
                     receive_receiver{&state, &context});
  auto send_operation =
      bexec::connect(client.async_send_to(scheduler, payload, destination),
                     send_receiver{&state, &context});
  bexec::start(receive_operation);
  bexec::start(send_operation);
  assert(scheduler.queued_io_size() == 1);
  assert(!scheduler.flush_io_queue());
  context.run();

  assert(!state.error);
  assert(state.sent == payload.size());
  assert(state.received == payload.size());
  assert(received == payload);
  assert(source.address().is_v6());
}

}  // namespace

int main() {
  test_sender_concepts();
  test_protocol_and_lifecycle();
  test_async_send_to_receive_from<false>();
  test_async_send_to_receive_from<true>();
  test_async_default_peer<false>();
  test_async_default_peer<true>();
  test_async_ipv6_send_to();
  return 0;
}
