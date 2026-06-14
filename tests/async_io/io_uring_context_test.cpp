#include <bupp/async_io/linux/io_uring_context.h>
#include <liburing.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <array>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <bexec/stop_token.hpp>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <system_error>
#include <thread>
#include <type_traits>

namespace {

using bupp::async_io::buffer_view;
using bupp::async_io::descriptor_view;
using bupp::async_io::listening_socket_view;
using bupp::async_io::stream_socket_view;
using bupp::async_io::linux_native::buffer_sequence_view;
using bupp::async_io::linux_native::const_message_view;
using bupp::async_io::linux_native::io_uring_accept_operation;
using bupp::async_io::linux_native::io_uring_connect_operation;
using bupp::async_io::linux_native::io_uring_context;
using bupp::async_io::linux_native::io_uring_context_options;
using bupp::async_io::linux_native::io_uring_nop_operation;
using bupp::async_io::linux_native::io_uring_operation_base;
using bupp::async_io::linux_native::io_uring_poll_operation;
using bupp::async_io::linux_native::io_uring_post_operation;
using bupp::async_io::linux_native::io_uring_read_operation;
using bupp::async_io::linux_native::io_uring_readv_operation;
using bupp::async_io::linux_native::io_uring_recv_operation;
using bupp::async_io::linux_native::io_uring_recvmsg_operation;
using bupp::async_io::linux_native::io_uring_resolve_operation;
using bupp::async_io::linux_native::io_uring_send_operation;
using bupp::async_io::linux_native::io_uring_sendmsg_operation;
using bupp::async_io::linux_native::io_uring_timeout_operation;
using bupp::async_io::linux_native::io_uring_write_operation;
using bupp::async_io::linux_native::io_uring_writev_operation;
using bupp::async_io::linux_native::mutable_message_view;

enum class signal_kind {
  none,
  value,
  error,
  stopped,
};

struct shared_state {
  signal_kind signal = signal_kind::none;
  int result = 0;
  unsigned flags = 0;
  bool in_context = false;
  std::error_code error;
  int prepare_result = -1;
};

struct receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  io_uring_context* context = nullptr;
  bool stop_on_completion = false;

  void set_value() noexcept {
    state->signal = signal_kind::value;
    state->in_context = context != nullptr && context->is_in_context();
    if (stop_on_completion && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_value(int result, unsigned flags) noexcept {
    state->signal = signal_kind::value;
    state->result = result;
    state->flags = flags;
    state->in_context = context != nullptr && context->is_in_context();
    if (stop_on_completion && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    state->in_context = context != nullptr && context->is_in_context();
    if (stop_on_completion && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    state->in_context = context != nullptr && context->is_in_context();
    if (stop_on_completion && context != nullptr) {
      (void)context->stop();
    }
  }
};

struct resolve_state {
  signal_kind signal = signal_kind::none;
  std::size_t endpoint_count = 0;
  bool in_context = false;
  std::error_code error;
};

struct resolve_receiver {
  resolve_state* state = nullptr;
  io_uring_context* context = nullptr;

  void set_value(std::size_t count) noexcept {
    if (state != nullptr) {
      state->signal = signal_kind::value;
      state->endpoint_count = count;
      state->in_context = context != nullptr && context->is_in_context();
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    if (state != nullptr) {
      state->signal = signal_kind::error;
      state->error = error;
      state->in_context = context != nullptr && context->is_in_context();
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    if (state != nullptr) {
      state->signal = signal_kind::stopped;
      state->in_context = context != nullptr && context->is_in_context();
    }
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

struct stopped_receiver : receiver {
  stop_env env;

  [[nodiscard]] stop_env get_env() const noexcept { return env; }
};

struct fill_nop_operation : io_uring_operation_base {
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_nop();
  }

  void execute() noexcept override {}
};

struct prepare_on_completion_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  io_uring_context* context = nullptr;
  fill_nop_operation* filler = nullptr;

  void set_value(int result, unsigned flags) noexcept {
    state->signal = signal_kind::value;
    state->result = result;
    state->flags = flags;
    state->in_context = context != nullptr && context->is_in_context();
    if (context != nullptr && filler != nullptr) {
      state->prepare_result = context->prepare(*filler);
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    state->in_context = context != nullptr && context->is_in_context();
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    state->in_context = context != nullptr && context->is_in_context();
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct batch_state {
  unsigned completed = 0;
  unsigned errors = 0;
  unsigned stopped = 0;
  bool all_in_context = true;
  bool in_order = true;
  unsigned next_index = 0;
};

struct batch_receiver {
  std::shared_ptr<batch_state> state = std::make_shared<batch_state>();
  io_uring_context* context = nullptr;
  unsigned target = 0;

  void set_value(int result, unsigned /*flags*/) noexcept {
    assert(result == 0);
    ++state->completed;
    state->all_in_context =
        state->all_in_context && context != nullptr && context->is_in_context();
    if (state->completed == target && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code /*error*/) noexcept {
    ++state->errors;
    state->all_in_context =
        state->all_in_context && context != nullptr && context->is_in_context();
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    ++state->stopped;
    state->all_in_context =
        state->all_in_context && context != nullptr && context->is_in_context();
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct post_batch_receiver {
  std::shared_ptr<batch_state> state = std::make_shared<batch_state>();
  io_uring_context* context = nullptr;
  unsigned target = 0;
  unsigned index = 0;

  void set_value() noexcept {
    state->in_order = state->in_order && index == state->next_index;
    ++state->next_index;
    ++state->completed;
    state->all_in_context =
        state->all_in_context && context != nullptr && context->is_in_context();
    if (state->completed == target && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code /*error*/) noexcept {
    ++state->errors;
    state->all_in_context =
        state->all_in_context && context != nullptr && context->is_in_context();
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    ++state->stopped;
    state->all_in_context =
        state->all_in_context && context != nullptr && context->is_in_context();
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

bool is_unsupported_ring_error(int result) {
  return result == -ENOSYS || result == -EPERM || result == -EACCES;
}

bool queue_init_or_skip(io_uring_context& context,
                        io_uring_context_options options = {}) {
  const int result = context.queue_init(options);
  if (result < 0) {
    assert(is_unsupported_ring_error(result));
    return false;
  }
  return true;
}

void test_operation_state_concepts() {
  static_assert(bexec::operation_state<io_uring_post_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_nop_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_timeout_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_poll_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_accept_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_connect_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_recv_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_send_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_recvmsg_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_sendmsg_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_read_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_write_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_readv_operation<receiver>>);
  static_assert(bexec::operation_state<io_uring_writev_operation<receiver>>);
  static_assert(
      bexec::operation_state<io_uring_resolve_operation<resolve_receiver>>);

  using resolve_sender =
      decltype(std::declval<io_uring_context&>().async_resolve(
          bupp::async_io::dns_query{}, bupp::async_io::dns_result_view{}));
  static_assert(bexec::sender<resolve_sender>);

  static_assert(
      !std::is_constructible_v<io_uring_accept_operation<receiver>,
                               io_uring_context&, stream_socket_view,
                               bupp::async_io::ip::endpoint&, int, receiver>);
  static_assert(
      !std::is_constructible_v<io_uring_connect_operation<receiver>,
                               io_uring_context&, listening_socket_view,
                               const bupp::async_io::ip::endpoint&, receiver>);
  static_assert(
      !std::is_constructible_v<io_uring_recv_operation<receiver>,
                               io_uring_context&, listening_socket_view,
                               const buffer_view&, int, receiver>);
  static_assert(
      !std::is_constructible_v<io_uring_send_operation<receiver>,
                               io_uring_context&, listening_socket_view,
                               const buffer_view&, int, receiver>);
}

void test_io_operations_accept_async_io_views() {
  io_uring_context context;
  listening_socket_view listener(3);
  stream_socket_view stream(4);
  descriptor_view descriptor(4);

  char data[16]{};
  buffer_view buffer{data, sizeof(data)};
  bupp::async_io::ip::endpoint remote_endpoint =
      bupp::async_io::ip::endpoint::loopback_v4(80);
  msghdr message{};
  mutable_message_view receive_message(message);
  const_message_view send_message(message);
  iovec vector{data, sizeof(data)};
  buffer_sequence_view vectors(&vector, 1);
  bupp::async_io::duration timeout{};

  [[maybe_unused]] io_uring_timeout_operation timeout_operation(
      context, timeout, 0, 0, receiver{});
  [[maybe_unused]] io_uring_timeout_operation chrono_timeout_operation(
      context, std::chrono::milliseconds(1), 0, 0, receiver{});
  [[maybe_unused]] io_uring_poll_operation poll_operation(context, descriptor,
                                                          0, receiver{});
  [[maybe_unused]] io_uring_accept_operation accept_operation(
      context, listener, remote_endpoint, 0, receiver{});
  [[maybe_unused]] io_uring_accept_operation accept_without_endpoint_operation(
      context, listener, 0, receiver{});
  [[maybe_unused]] io_uring_connect_operation connect_operation(
      context, stream, remote_endpoint, receiver{});
  [[maybe_unused]] io_uring_recv_operation recv_operation(
      context, stream, buffer, 0, receiver{});
  [[maybe_unused]] io_uring_send_operation send_operation(
      context, stream, buffer, 0, receiver{});
  [[maybe_unused]] io_uring_recvmsg_operation recvmsg_operation(
      context, stream, receive_message, 0, receiver{});
  [[maybe_unused]] io_uring_sendmsg_operation sendmsg_operation(
      context, stream, send_message, 0, receiver{});
  [[maybe_unused]] io_uring_read_operation read_operation(
      context, descriptor, buffer, 0, receiver{});
  [[maybe_unused]] io_uring_write_operation write_operation(
      context, descriptor, buffer, 0, receiver{});
  [[maybe_unused]] io_uring_readv_operation readv_operation(
      context, descriptor, vectors, 0, receiver{});
  [[maybe_unused]] io_uring_writev_operation writev_operation(
      context, descriptor, vectors, 0, receiver{});
  bupp::async_io::ip::endpoint resolved_endpoints[4]{};
  [[maybe_unused]] io_uring_resolve_operation resolve_operation(
      context, bupp::async_io::dns_query("127.0.0.1", "80"),
      bupp::async_io::dns_result_view(resolved_endpoints), resolve_receiver{});
}

void test_timeout_operation_prepares_async_io_time() {
  io_uring_context context;
  io_uring_sqe raw_sqe{};
  io_uring_initialize_sqe(&raw_sqe);
  bupp::base::submission_queue_entry sqe(&raw_sqe);

  io_uring_timeout_operation operation(
      context, std::chrono::seconds(2) + std::chrono::nanoseconds(5), 3,
      IORING_TIMEOUT_ABS, receiver{});
  operation.prepare(sqe);

  const auto* timeout = reinterpret_cast<const __kernel_timespec*>(
      static_cast<std::uintptr_t>(raw_sqe.addr));
  assert(raw_sqe.opcode == IORING_OP_TIMEOUT);
  assert(raw_sqe.timeout_flags == IORING_TIMEOUT_ABS);
  assert(timeout != nullptr);
  assert(timeout->tv_sec == 2);
  assert(timeout->tv_nsec == 5);
}

void test_post_operation_runs_on_context_thread() {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    return;
  }

  receiver recv;
  recv.context = &context;
  recv.stop_on_completion = true;
  auto state = recv.state;

  io_uring_post_operation operation(context, std::move(recv));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->in_context);
}

void test_resolve_sender_runs_on_context_thread() {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    return;
  }

  bupp::async_io::dns_query query("127.0.0.1", "8080");
  query.set_address_version(bupp::async_io::ip::address::version::v4);
  std::array<bupp::async_io::ip::endpoint, 8> results{};

  resolve_receiver recv;
  resolve_state state;
  recv.state = &state;
  recv.context = &context;

  auto sender = context.async_resolve(std::move(query),
                                      bupp::async_io::dns_result_view(results));
  auto operation = bexec::connect(std::move(sender), std::move(recv));
  bexec::start(operation);
  context.run();

  assert(state.signal == signal_kind::value);
  assert(state.endpoint_count > 0);
  assert(results[0].port() == 8080);
  assert(results[0].address().type() ==
         bupp::async_io::ip::address::version::v4);
  assert(state.in_context);
}

void test_nop_operation_completes_with_raw_cqe() {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    return;
  }

  receiver recv;
  recv.context = &context;
  recv.stop_on_completion = true;
  auto state = recv.state;

  io_uring_nop_operation operation(context, std::move(recv));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->result == 0);
  assert(state->in_context);
}

void test_stop_token_completes_stopped_before_submit() {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    return;
  }

  bexec::inplace_stop_source source;
  assert(source.request_stop());

  stopped_receiver recv;
  recv.context = &context;
  recv.env = stop_env{source.get_token()};
  recv.stop_on_completion = true;
  auto state = recv.state;

  io_uring_nop_operation operation(context, std::move(recv));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::stopped);
  assert(state->in_context);
}

void test_cpu_post_wakes_blocked_worker() {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    return;
  }

  std::thread worker([&context] { context.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(25));

  receiver recv;
  recv.context = &context;
  recv.stop_on_completion = true;
  auto state = recv.state;

  io_uring_post_operation operation(context, std::move(recv));
  bexec::start(operation);
  worker.join();

  assert(state->signal == signal_kind::value);
  assert(state->in_context);
}

void test_global_post_wakes_waiting_worker() {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    return;
  }

  std::thread io_worker([&context] { context.run(); });
  std::thread cv_worker([&context] { context.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(25));

  receiver recv;
  recv.context = &context;
  recv.stop_on_completion = true;
  auto state = recv.state;

  io_uring_post_operation operation(context, std::move(recv));
  bexec::start(operation);

  io_worker.join();
  cv_worker.join();

  assert(state->signal == signal_kind::value);
  assert(state->in_context);
}

void test_global_posts_drain_in_post_order() {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    return;
  }

  constexpr unsigned k_count = 8;
  auto state = std::make_shared<batch_state>();
  std::array<std::unique_ptr<io_uring_post_operation<post_batch_receiver>>,
             k_count>
      operations;

  for (unsigned index = 0; index < k_count; ++index) {
    post_batch_receiver recv;
    recv.context = &context;
    recv.target = k_count;
    recv.index = index;
    recv.state = state;
    operations[index] =
        std::make_unique<io_uring_post_operation<post_batch_receiver>>(
            context, std::move(recv));
    bexec::start(*operations[index]);
  }

  context.run();

  assert(state->completed == k_count);
  assert(state->errors == 0);
  assert(state->stopped == 0);
  assert(state->all_in_context);
  assert(state->in_order);
}

void test_cqe_completion_runs_without_uring_lock() {
  fill_nop_operation filler;
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    return;
  }

  prepare_on_completion_receiver recv;
  recv.context = &context;
  recv.filler = &filler;
  auto state = recv.state;

  io_uring_nop_operation operation(context, std::move(recv));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::value);
  assert(state->result == 0);
  assert(state->prepare_result == 0);
  assert(state->in_context);
}

void test_cqe_batch_window_drains_multiple_rounds() {
  io_uring_context context;
  io_uring_context_options options;
  options.entries = 16;
  options.cqe_batch_window = 2;
  options.wait_spin_count = 1;
  options.cqe_inline_completion_threshold = 0;
  if (!queue_init_or_skip(context, options)) {
    return;
  }

  constexpr unsigned k_count = 5;
  auto state = std::make_shared<batch_state>();
  std::array<std::unique_ptr<io_uring_nop_operation<batch_receiver>>, k_count>
      operations;

  for (auto& operation : operations) {
    batch_receiver recv;
    recv.context = &context;
    recv.target = k_count;
    recv.state = state;
    operation = std::make_unique<io_uring_nop_operation<batch_receiver>>(
        context, std::move(recv));
    bexec::start(*operation);
  }

  context.run();

  assert(state->completed == k_count);
  assert(state->errors == 0);
  assert(state->stopped == 0);
  assert(state->all_in_context);
}

void test_submit_failure_posts_error_completion() {
  io_uring_context context;
  io_uring_context_options options;
  options.entries = 2;
  if (!queue_init_or_skip(context, options)) {
    return;
  }

  std::array<fill_nop_operation, 64> fillers;
  bool full = false;
  for (fill_nop_operation& filler : fillers) {
    const int result = context.prepare(filler);
    if (result == -EAGAIN) {
      full = true;
      break;
    }
    assert(result == 0);
  }
  if (!full) {
    return;
  }

  receiver recv;
  recv.context = &context;
  recv.stop_on_completion = true;
  auto state = recv.state;

  io_uring_nop_operation operation(context, std::move(recv));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::error);
  assert(state->error == std::error_code(EAGAIN, std::generic_category()));
  assert(state->in_context);
}

}  // namespace

int main() {
  test_operation_state_concepts();
  test_io_operations_accept_async_io_views();
  test_timeout_operation_prepares_async_io_time();
  test_post_operation_runs_on_context_thread();
  test_resolve_sender_runs_on_context_thread();
  test_nop_operation_completes_with_raw_cqe();
  test_stop_token_completes_stopped_before_submit();
  test_cpu_post_wakes_blocked_worker();
  test_global_post_wakes_waiting_worker();
  test_global_posts_drain_in_post_order();
  test_cqe_completion_runs_without_uring_lock();
  test_cqe_batch_window_drains_multiple_rounds();
  test_submit_failure_posts_error_completion();
  return 0;
}
