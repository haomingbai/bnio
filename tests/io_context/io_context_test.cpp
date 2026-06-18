#include <bupp/io_context.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <bexec/operation_state.hpp>
#include <bexec/scheduler.hpp>
#include <bexec/sender.hpp>
#include <bexec/stop_token.hpp>
#include <cassert>
#include <chrono>
#include <cstring>
#include <memory>
#include <string_view>
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

struct shared_state {
  signal_kind signal = signal_kind::none;
  std::size_t size = 0;
  std::error_code error;
};

struct schedule_state {
  signal_kind signal = signal_kind::none;
  std::vector<int> order;
  unsigned completions = 0;
  bool completed_during_start = false;
};

struct byte_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bupp::io_context* context = nullptr;

  void set_value(std::size_t size) noexcept {
    state->signal = signal_kind::value;
    state->size = size;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct void_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bupp::io_context* context = nullptr;
  unsigned* completions = nullptr;
  unsigned target = 1;

  void set_value() noexcept {
    state->signal = signal_kind::value;
    if (completions != nullptr) {
      ++*completions;
      if (*completions != target) {
        return;
      }
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    if (completions != nullptr) {
      ++*completions;
      if (*completions != target) {
        return;
      }
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct poll_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bupp::io_context* context = nullptr;

  void set_value(unsigned events) noexcept {
    state->signal = signal_kind::value;
    state->size = events;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct stop_env {
  bexec::inplace_stop_token token;

  [[nodiscard]] bexec::inplace_stop_token query(
      bexec::get_stop_token_t) const noexcept {
    return token;
  }
};

struct stopped_void_receiver : void_receiver {
  stop_env env;

  [[nodiscard]] stop_env get_env() const noexcept { return env; }
};

struct schedule_receiver {
  std::shared_ptr<schedule_state> state = std::make_shared<schedule_state>();
  bupp::io_context* context = nullptr;
  int value = 0;
  unsigned target = 1;

  void set_value() noexcept {
    state->signal = signal_kind::value;
    state->order.push_back(value);
    ++state->completions;
    if (context != nullptr && state->completions == target) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    (void)error;
    state->signal = signal_kind::error;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    ++state->completions;
    if (context != nullptr && state->completions == target) {
      (void)context->stop();
    }
  }
};

struct stopped_schedule_receiver : schedule_receiver {
  stop_env env;

  [[nodiscard]] stop_env get_env() const noexcept { return env; }
};

struct dispatch_inline_outer_receiver {
  std::shared_ptr<schedule_state> state;
  bupp::io_context* context = nullptr;

  void set_value() noexcept {
    schedule_receiver inner;
    inner.state = state;
    inner.value = 42;

    auto sender = bexec::schedule(context->get_dispatch_scheduler());
    auto operation = bexec::connect(std::move(sender), std::move(inner));
    bexec::start(operation);

    state->completed_during_start = state->signal == signal_kind::value;
    (void)context->stop();
  }

  void set_error(std::error_code error) noexcept {
    (void)error;
    state->signal = signal_kind::error;
    (void)context->stop();
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    (void)context->stop();
  }
};

[[nodiscard]] bool context_available(const bupp::io_context& context) {
  return context.is_open();
}

void test_sender_concepts() {
  bupp::io_context context;
  auto scheduler = context.get_post_scheduler();
  bupp::tcp_socket socket(3);
  std::array<char, 8> bytes{};

  using receive_sender =
      decltype(scheduler.async_receive(socket, bupp::buffer(bytes)));
  using send_sender =
      decltype(scheduler.async_send(socket, std::string_view("abc")));
  using poll_sender = decltype(scheduler.async_poll(
      bupp::async_io::descriptor_view(3), static_cast<unsigned>(POLLIN)));
  using schedule_sender = decltype(bexec::schedule(scheduler));
  using timer_wait_sender =
      decltype(std::declval<bupp::steady_timer&>().async_wait());
  static_assert(bexec::sender<receive_sender>);
  static_assert(bexec::sender<send_sender>);
  static_assert(bexec::sender<poll_sender>);
  static_assert(bexec::sender<schedule_sender>);
  static_assert(bexec::sender<timer_wait_sender>);
  static_assert(bexec::scheduler<bupp::io_context::dispatch_scheduler>);
  static_assert(bexec::scheduler<bupp::io_context::post_scheduler>);
  static_assert(bupp::receives_bytes<bupp::io_context::post_scheduler,
                                     bupp::tcp_socket, bupp::mutable_buffer>);
  static_assert(bupp::receives_bytes<bupp::io_context::dispatch_scheduler,
                                     bupp::tcp_socket, bupp::mutable_buffer>);
  static_assert(bupp::sends_bytes<bupp::io_context::post_scheduler,
                                  bupp::tcp_socket, bupp::const_buffer>);
  static_assert(bupp::sends_bytes<bupp::io_context::dispatch_scheduler,
                                  bupp::tcp_socket, bupp::const_buffer>);
  static_assert(bupp::accepts_connections<bupp::io_context::post_scheduler,
                                          bupp::tcp_acceptor>);
  static_assert(
      bupp::connects_stream<bupp::io_context::post_scheduler, bupp::tcp_socket,
                            const bupp::ip::endpoint&>);
  static_assert(bupp::polls_descriptor<bupp::io_context::post_scheduler,
                                       bupp::async_io::descriptor_view>);

  byte_receiver receiver;
  auto sender = scheduler.async_receive(socket, bupp::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  static_assert(bexec::operation_state<decltype(operation)>);

  (void)socket.release();
}

void test_queued_poll_observes_pipe_readiness() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int descriptors[2] = {-1, -1};
  assert(::pipe2(descriptors, O_CLOEXEC) == 0);

  poll_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = bupp::async_poll(
      scheduler, bupp::async_io::descriptor_view(descriptors[0]),
      static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  assert(scheduler.queued_io_size() == 1);

  constexpr char byte = 'q';
  assert(::write(descriptors[1], &byte, sizeof(byte)) ==
         static_cast<ssize_t>(sizeof(byte)));
  assert(!scheduler.flush_io_queue());
  context.run();

  assert(state->signal == signal_kind::value);
  assert((static_cast<unsigned>(state->size) & static_cast<unsigned>(POLLIN)) !=
         0);

  assert(::close(descriptors[0]) == 0);
  assert(::close(descriptors[1]) == 0);
}

void test_direct_poll_submits_without_queue() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int descriptors[2] = {-1, -1};
  assert(::pipe2(descriptors, O_CLOEXEC) == 0);

  poll_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = bupp::async_poll_direct(
      scheduler, bupp::async_io::descriptor_view(descriptors[0]),
      static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  assert(scheduler.queued_io_size() == 0);

  constexpr char byte = 'd';
  assert(::write(descriptors[1], &byte, sizeof(byte)) ==
         static_cast<ssize_t>(sizeof(byte)));
  context.run();

  assert(state->signal == signal_kind::value);
  assert((static_cast<unsigned>(state->size) & static_cast<unsigned>(POLLIN)) !=
         0);

  assert(::close(descriptors[0]) == 0);
  assert(::close(descriptors[1]) == 0);
}

void test_manual_flush_receives_queued_io() {
  bupp::io_context_options options;
  options.platform.max_queued_io_operations = 64;
  options.platform.queued_io_flush_after = std::chrono::seconds(30);
  bupp::io_context context(options);
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  bupp::tcp_socket receiver_socket(sockets[0]);
  bupp::tcp_socket sender_socket(sockets[1]);

  std::array<char, 16> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = scheduler.async_receive(receiver_socket, bupp::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  assert(scheduler.queued_io_size() == 1);

  constexpr std::string_view payload = "queued";
  assert(::send(sender_socket.native_handle(), payload.data(), payload.size(),
                MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size()));

  assert(!scheduler.flush_io_queue());
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->size == payload.size());
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

void test_direct_receive_submits_without_queue() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  bupp::tcp_socket receiver_socket(sockets[0]);
  bupp::tcp_socket sender_socket(sockets[1]);

  std::array<char, 16> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender =
      scheduler.async_receive_direct(receiver_socket, bupp::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  assert(scheduler.queued_io_size() == 0);

  constexpr std::string_view payload = "direct";
  assert(::send(sender_socket.native_handle(), payload.data(), payload.size(),
                MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size()));

  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->size == payload.size());
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

void test_queued_io_auto_flush_timer_receives() {
  bupp::io_context_options options;
  options.platform.max_queued_io_operations = 64;
  options.platform.queued_io_flush_after = std::chrono::milliseconds(1);
  bupp::io_context context(options);
  if (!context_available(context)) {
    return;
  }
  auto scheduler = context.get_post_scheduler();

  int sockets[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
  bupp::tcp_socket receiver_socket(sockets[0]);
  bupp::tcp_socket sender_socket(sockets[1]);

  std::array<char, 16> bytes{};
  byte_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = scheduler.async_receive(receiver_socket, bupp::buffer(bytes));
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  assert(scheduler.queued_io_size() == 1);

  constexpr std::string_view payload = "auto";
  assert(::send(sender_socket.native_handle(), payload.data(), payload.size(),
                MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size()));

  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->size == payload.size());
  assert(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
}

void test_post_scheduler_schedule_posts_fifo() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  auto scheduler = context.get_post_scheduler();
  auto state = std::make_shared<schedule_state>();
  state->order.reserve(3);

  schedule_receiver first;
  first.state = state;
  first.context = &context;
  first.value = 1;
  first.target = 3;

  schedule_receiver second;
  second.state = state;
  second.context = &context;
  second.value = 2;
  second.target = 3;

  schedule_receiver third;
  third.state = state;
  third.context = &context;
  third.value = 3;
  third.target = 3;

  auto first_operation =
      bexec::connect(bexec::schedule(scheduler), std::move(first));
  auto second_operation =
      bexec::connect(bexec::schedule(scheduler), std::move(second));
  auto third_operation =
      bexec::connect(bexec::schedule(scheduler), std::move(third));

  bexec::start(first_operation);
  bexec::start(second_operation);
  bexec::start(third_operation);

  assert(state->order.empty());
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->order.size() == 3);
  assert(state->order[0] == 1);
  assert(state->order[1] == 2);
  assert(state->order[2] == 3);
}

void test_dispatch_scheduler_schedule_posts_outside_context() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  auto scheduler = context.get_dispatch_scheduler();
  auto state = std::make_shared<schedule_state>();
  state->order.reserve(1);

  schedule_receiver receiver;
  receiver.state = state;
  receiver.context = &context;
  receiver.value = 7;

  auto operation =
      bexec::connect(bexec::schedule(scheduler), std::move(receiver));
  bexec::start(operation);

  assert(state->signal == signal_kind::none);
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->order.size() == 1);
  assert(state->order[0] == 7);
}

void test_dispatch_scheduler_schedule_runs_inline_in_context() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  auto state = std::make_shared<schedule_state>();
  state->order.reserve(1);

  auto operation =
      bexec::connect(bexec::schedule(context.get_post_scheduler()),
                     dispatch_inline_outer_receiver{state, &context});
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->completed_during_start);
  assert(state->order.size() == 1);
  assert(state->order[0] == 42);
}

void test_scheduler_schedule_pre_stopped_token_stops() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  bexec::inplace_stop_source source;
  assert(source.request_stop());

  auto state = std::make_shared<schedule_state>();
  stopped_schedule_receiver receiver;
  receiver.state = state;
  receiver.context = &context;
  receiver.env = stop_env{source.get_token()};

  auto operation = bexec::connect(bexec::schedule(context.get_post_scheduler()),
                                  std::move(receiver));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::stopped);
}

void test_steady_timer_completes() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  bupp::steady_timer timer(context);
  assert(timer.expires_after(std::chrono::milliseconds(1)) == 0);

  void_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = timer.async_wait();
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::value);
}

void test_steady_timer_cancel_stops_wait() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  bupp::steady_timer timer(context);
  assert(timer.expires_after(std::chrono::seconds(30)) == 0);

  void_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = timer.async_wait();
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  assert(timer.cancel() == 1);
  context.run();

  assert(state->signal == signal_kind::stopped);
}

void test_steady_timer_expires_after_stops_old_wait() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  bupp::steady_timer timer(context);
  assert(timer.expires_after(std::chrono::seconds(30)) == 0);

  void_receiver receiver;
  receiver.context = &context;
  auto state = receiver.state;

  auto sender = timer.async_wait();
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  assert(timer.expires_after(std::chrono::milliseconds(1)) == 1);
  context.run();

  assert(state->signal == signal_kind::stopped);
}

void test_steady_timer_multiple_waits_complete() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  bupp::steady_timer timer(context);
  assert(timer.expires_after(std::chrono::milliseconds(1)) == 0);

  unsigned completions = 0;
  void_receiver first;
  first.context = &context;
  first.completions = &completions;
  first.target = 2;
  auto first_state = first.state;

  void_receiver second;
  second.context = &context;
  second.completions = &completions;
  second.target = 2;
  auto second_state = second.state;

  auto first_sender = timer.async_wait();
  auto second_sender = timer.async_wait();
  auto first_operation =
      bexec::connect(std::move(first_sender), std::move(first));
  auto second_operation =
      bexec::connect(std::move(second_sender), std::move(second));
  bexec::start(first_operation);
  bexec::start(second_operation);
  context.run();

  assert(completions == 2);
  assert(first_state->signal == signal_kind::value);
  assert(second_state->signal == signal_kind::value);
}

void test_steady_timer_move_stops_old_wait() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  bupp::steady_timer timer(context);
  assert(timer.expires_after(std::chrono::seconds(30)) == 0);

  unsigned completions = 0;
  void_receiver receiver;
  receiver.context = &context;
  receiver.completions = &completions;
  receiver.target = 2;
  auto state = receiver.state;

  auto sender = timer.async_wait();
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);

  bupp::steady_timer moved_timer(std::move(timer));
  assert(moved_timer.expires_after(std::chrono::milliseconds(1)) == 0);

  void_receiver moved_receiver;
  moved_receiver.context = &context;
  moved_receiver.completions = &completions;
  moved_receiver.target = 2;
  auto moved_state = moved_receiver.state;
  auto moved_sender = moved_timer.async_wait();
  auto moved_operation =
      bexec::connect(std::move(moved_sender), std::move(moved_receiver));
  bexec::start(moved_operation);
  context.run();

  assert(completions == 2);
  assert(state->signal == signal_kind::stopped);
  assert(moved_state->signal == signal_kind::value);
}

void test_steady_timer_pre_stopped_token_stops_wait() {
  bupp::io_context context;
  if (!context_available(context)) {
    return;
  }

  bexec::inplace_stop_source source;
  assert(source.request_stop());
  bupp::steady_timer timer(context);
  assert(timer.expires_after(std::chrono::seconds(30)) == 0);

  stopped_void_receiver receiver;
  receiver.context = &context;
  receiver.env = stop_env{source.get_token()};
  auto state = receiver.state;

  auto sender = timer.async_wait();
  auto operation = bexec::connect(std::move(sender), std::move(receiver));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::stopped);
}

}  // namespace

int main() {
  test_sender_concepts();
  test_queued_poll_observes_pipe_readiness();
  test_direct_poll_submits_without_queue();
  test_manual_flush_receives_queued_io();
  test_direct_receive_submits_without_queue();
  test_queued_io_auto_flush_timer_receives();
  test_post_scheduler_schedule_posts_fifo();
  test_dispatch_scheduler_schedule_posts_outside_context();
  test_dispatch_scheduler_schedule_runs_inline_in_context();
  test_scheduler_schedule_pre_stopped_token_stops();
  test_steady_timer_completes();
  test_steady_timer_cancel_stops_wait();
  test_steady_timer_expires_after_stops_old_wait();
  test_steady_timer_multiple_waits_complete();
  test_steady_timer_move_stops_old_wait();
  test_steady_timer_pre_stopped_token_stops_wait();
  return 0;
}
