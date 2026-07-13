#include <atomic>
#include <barrier>
#include <bexec/operation_state.hpp>
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "kqueue_context_test_support.h"

namespace {

using namespace bupp_async_io_kqueue_test;

struct concurrent_state {
  std::atomic<unsigned> completed{0};
  std::atomic<unsigned> stopped{0};
  std::atomic_bool all_in_context{true};
};

struct concurrent_receiver {
  std::shared_ptr<concurrent_state> state;
  kqueue_context* context = nullptr;
  unsigned target = 0;

  void set_value() noexcept {
    if (context == nullptr || !context->is_in_context()) {
      state->all_in_context.store(false, std::memory_order_release);
    }
    const unsigned completed =
        state->completed.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (completed == target && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->stopped.fetch_add(1, std::memory_order_acq_rel);
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct batch_receiver {
  std::shared_ptr<concurrent_state> state;
  kqueue_context* context = nullptr;
  unsigned target = 0;

  void set_value(int result, unsigned /*flags*/) noexcept {
    assert(result == 0);
    const unsigned completed =
        state->completed.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (completed == target && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code /*error*/) noexcept {
    state->stopped.fetch_add(1, std::memory_order_acq_rel);
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->stopped.fetch_add(1, std::memory_order_acq_rel);
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

void test_submit_batch_registers_all_prepared_operations() {
  kqueue_context context;
  kqueue_context_options options;
  options.entries = 8;
  options.event_inline_completion_threshold = 0;
  options.local_queue_threshold = 2;
  assert(context.queue_init(options) == 0);

  constexpr unsigned operation_count = 8;
  auto state = std::make_shared<concurrent_state>();
  std::vector<std::unique_ptr<kqueue_nop_operation<batch_receiver>>> operations;
  operations.reserve(operation_count);
  for (unsigned index = 0; index < operation_count; ++index) {
    batch_receiver completion{state, &context, operation_count};
    operations.push_back(std::make_unique<kqueue_nop_operation<batch_receiver>>(
        context, std::move(completion)));
  }

  context.submit_batch([&operations](auto prepare, auto submit) noexcept {
    for (auto& operation : operations) {
      assert(prepare(*operation) == 0);
    }
    assert(submit() == static_cast<int>(operations.size()));
  });
  context.run();

  assert(state->completed.load(std::memory_order_acquire) == operation_count);
  assert(state->stopped.load(std::memory_order_acquire) == 0);
}

void test_concurrent_external_posts_are_drained() {
  kqueue_context context;
  kqueue_context_options options;
  options.wait_spin_count = 1;
  assert(context.queue_init(options) == 0);

  constexpr unsigned thread_count = 4;
  constexpr unsigned posts_per_thread = 128;
  constexpr unsigned operation_count = thread_count * posts_per_thread;
  auto state = std::make_shared<concurrent_state>();

  std::vector<std::unique_ptr<kqueue_post_operation<concurrent_receiver>>>
      operations;
  operations.reserve(operation_count);
  for (unsigned index = 0; index < operation_count; ++index) {
    concurrent_receiver completion{state, &context, operation_count};
    operations.push_back(
        std::make_unique<kqueue_post_operation<concurrent_receiver>>(
            context, std::move(completion)));
  }

  std::thread runner([&context] { context.run(); });
  std::barrier ready(static_cast<std::ptrdiff_t>(thread_count + 1));
  std::vector<std::thread> producers;
  producers.reserve(thread_count);
  for (unsigned thread = 0; thread < thread_count; ++thread) {
    producers.emplace_back([&operations, &ready, thread] {
      ready.arrive_and_wait();
      const unsigned first = thread * posts_per_thread;
      const unsigned last = first + posts_per_thread;
      for (unsigned index = first; index < last; ++index) {
        bexec::start(*operations[index]);
      }
    });
  }

  ready.arrive_and_wait();
  for (std::thread& producer : producers) {
    producer.join();
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (state->completed.load(std::memory_order_acquire) != operation_count &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (state->completed.load(std::memory_order_acquire) != operation_count) {
    (void)context.stop();
  }
  runner.join();

  assert(state->completed.load(std::memory_order_acquire) == operation_count);
  assert(state->stopped.load(std::memory_order_acquire) == 0);
  assert(state->all_in_context.load(std::memory_order_acquire));
}

}  // namespace

int main() {
  test_submit_batch_registers_all_prepared_operations();
  test_concurrent_external_posts_are_drained();
  return 0;
}
