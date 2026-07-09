#include <array>
#include <cassert>
#include <system_error>

#include "io_uring_context_test_support.h"

namespace {

using namespace bupp_async_io_io_uring_test;

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

void test_poll_sender_observes_pipe_readiness() {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    return;
  }

  int descriptors[2] = {-1, -1};
  assert(::pipe2(descriptors, O_CLOEXEC) == 0);

  poll_receiver recv;
  recv.context = &context;
  auto state = recv.state;

  auto sender = context.async_poll(descriptor_view(descriptors[0]),
                                   static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(recv));
  bexec::start(operation);

  constexpr char byte = 'x';
  assert(::write(descriptors[1], &byte, sizeof(byte)) ==
         static_cast<ssize_t>(sizeof(byte)));
  context.run();

  assert(state->signal == signal_kind::value);
  assert((static_cast<unsigned>(state->result) &
          static_cast<unsigned>(POLLIN)) != 0);
  assert(state->in_context);

  assert(::close(descriptors[0]) == 0);
  assert(::close(descriptors[1]) == 0);
}

void test_poll_sender_reports_bad_descriptor() {
  io_uring_context context;
  if (!queue_init_or_skip(context)) {
    return;
  }

  poll_receiver recv;
  recv.context = &context;
  auto state = recv.state;

  auto sender =
      context.async_poll(descriptor_view(-1), static_cast<unsigned>(POLLIN));
  auto operation = bexec::connect(std::move(sender), std::move(recv));
  bexec::start(operation);
  context.run();

  assert(state->signal == signal_kind::error);
  assert(state->error == std::error_code(EBADF, std::generic_category()));
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

}  // namespace

int main() {
  test_post_operation_runs_on_context_thread();
  test_poll_sender_observes_pipe_readiness();
  test_poll_sender_reports_bad_descriptor();
  test_resolve_sender_runs_on_context_thread();
  test_nop_operation_completes_with_raw_cqe();
  test_stop_token_completes_stopped_before_submit();
  return 0;
}
