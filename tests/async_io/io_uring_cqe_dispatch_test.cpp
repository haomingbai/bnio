#include <array>
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "io_uring_context_test_support.h"

namespace {

using namespace bupp_async_io_io_uring_test;

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

void test_multithreaded_cqe_dispatch_with_local_queue_threshold() {
  io_uring_context context;
  io_uring_context_options options;
  options.entries = 1024;
  options.cqe_batch_window = 1;
  options.wait_spin_count = 1024;
  options.cqe_inline_completion_threshold = 0;
  options.local_queue_threshold = 8;
  if (!queue_init_or_skip(context, options)) {
    return;
  }

  constexpr unsigned k_count = 512;
  constexpr unsigned k_threads = 4;
  auto state = std::make_shared<concurrent_batch_state>();

  std::barrier ready(static_cast<std::ptrdiff_t>(k_threads + 1));
  std::vector<std::thread> workers;
  workers.reserve(k_threads);
  for (unsigned index = 0; index < k_threads; ++index) {
    workers.emplace_back([&context, &ready] {
      ready.arrive_and_wait();
      context.run();
    });
  }

  ready.arrive_and_wait();
  std::this_thread::sleep_for(std::chrono::milliseconds(25));

  std::vector<
      std::unique_ptr<io_uring_nop_operation<concurrent_batch_receiver>>>
      operations;
  operations.reserve(k_count);
  for (unsigned index = 0; index < k_count; ++index) {
    concurrent_batch_receiver recv;
    recv.context = &context;
    recv.target = k_count;
    recv.state = state;
    operations.push_back(
        std::make_unique<io_uring_nop_operation<concurrent_batch_receiver>>(
            context, std::move(recv)));
    bexec::start(*operations.back());
  }

  for (auto& worker : workers) {
    worker.join();
  }

  assert(state->completed.load(std::memory_order_acquire) == k_count);
  assert(state->errors.load(std::memory_order_acquire) == 0);
  assert(state->stopped.load(std::memory_order_acquire) == 0);
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
  test_cqe_completion_runs_without_uring_lock();
  test_cqe_batch_window_drains_multiple_rounds();
  test_multithreaded_cqe_dispatch_with_local_queue_threshold();
  test_submit_failure_posts_error_completion();
  return 0;
}
