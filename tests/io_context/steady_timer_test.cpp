#include <thread>
#include <vector>

#include "io_context_runtime_test_support.h"

namespace {

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

void test_timer_update_stays_on_primary_ring_with_multiple_workers() {
  constexpr unsigned worker_count = 4;
  bupp::io_context_options options;
  options.concurrency_hint = worker_count;
  bupp::io_context context(options);
  if (!context_available(context)) {
    return;
  }

  unsigned completions = 0;
  bupp::steady_timer first_timer(context);
  assert(first_timer.expires_after(std::chrono::milliseconds(60)) == 0);

  void_receiver first_receiver;
  first_receiver.context = &context;
  first_receiver.completions = &completions;
  first_receiver.target = 2;
  auto first_state = first_receiver.state;
  auto first_operation =
      bexec::connect(first_timer.async_wait(), std::move(first_receiver));
  bexec::start(first_operation);

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (unsigned index = 0; index < worker_count; ++index) {
    workers.emplace_back([&context] { context.run(); });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  bupp::steady_timer second_timer(context);
  assert(second_timer.expires_after(std::chrono::milliseconds(5)) == 0);

  void_receiver second_receiver;
  second_receiver.context = &context;
  second_receiver.completions = &completions;
  second_receiver.target = 2;
  auto second_state = second_receiver.state;
  auto second_operation =
      bexec::connect(second_timer.async_wait(), std::move(second_receiver));
  bexec::start(second_operation);

  for (std::thread& worker : workers) {
    worker.join();
  }

  assert(completions == 2);
  assert(first_state->signal == signal_kind::value);
  assert(second_state->signal == signal_kind::value);
}

}  // namespace

int main() {
  test_steady_timer_completes();
  test_steady_timer_cancel_stops_wait();
  test_steady_timer_expires_after_stops_old_wait();
  test_steady_timer_multiple_waits_complete();
  test_steady_timer_move_stops_old_wait();
  test_steady_timer_pre_stopped_token_stops_wait();
  test_timer_update_stays_on_primary_ring_with_multiple_workers();
  return 0;
}
