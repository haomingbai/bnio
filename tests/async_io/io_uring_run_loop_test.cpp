#include <array>
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

#include "io_uring_context_test_support.h"

namespace {

using namespace bupp_async_io_io_uring_test;

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

}  // namespace

int main() {
  test_cpu_post_wakes_blocked_worker();
  test_global_post_wakes_waiting_worker();
  test_global_posts_drain_in_post_order();
  return 0;
}
