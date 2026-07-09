#include <atomic>
#include <memory>
#include <thread>

#include "io_context_runtime_test_support.h"

namespace {

struct stress_state {
  std::atomic<unsigned> completions{0};
  std::atomic<unsigned> stopped{0};
  std::atomic<unsigned> errors{0};
  unsigned target = 0;
  bupp::io_context* context = nullptr;
};

struct stress_receiver {
  std::shared_ptr<stress_state> state;

  void set_value() noexcept {
    const unsigned value =
        state->completions.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (value == state->target) {
      (void)state->context->stop();
    }
  }

  void set_error(std::error_code) noexcept {
    state->errors.fetch_add(1, std::memory_order_acq_rel);
    (void)state->context->stop();
  }

  void set_stopped() noexcept {
    state->stopped.fetch_add(1, std::memory_order_acq_rel);
    (void)state->context->stop();
  }
};

void test_multithreaded_post_stress() {
  constexpr unsigned k_workers = 4;
  constexpr unsigned k_producers = 4;
  constexpr unsigned k_operations_per_producer = 2048;
  constexpr unsigned k_total = k_producers * k_operations_per_producer;

  bupp::io_context_options options;
  options.concurrency_hint = k_workers;
  bupp::io_context context(options);
  if (!context_available(context)) {
    return;
  }

  auto state = std::make_shared<stress_state>();
  state->target = k_total;
  state->context = &context;

  using sender_type = decltype(bexec::schedule(context.get_post_scheduler()));
  using operation_type = sender_type::operation<stress_receiver>;

  std::vector<std::unique_ptr<operation_type>> operations;
  operations.reserve(k_total);
  for (unsigned index = 0; index < k_total; ++index) {
    operations.push_back(
        std::make_unique<operation_type>(context, stress_receiver{state}));
  }

  std::vector<std::thread> workers;
  workers.reserve(k_workers);
  for (unsigned index = 0; index < k_workers; ++index) {
    workers.emplace_back([&context] { context.run(); });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(25));

  std::vector<std::thread> producers;
  producers.reserve(k_producers);
  for (unsigned producer = 0; producer < k_producers; ++producer) {
    producers.emplace_back([producer, &operations] {
      const unsigned begin = producer * k_operations_per_producer;
      const unsigned end = begin + k_operations_per_producer;
      for (unsigned index = begin; index < end; ++index) {
        operations[index]->start();
      }
    });
  }

  for (std::thread& producer : producers) {
    producer.join();
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  assert(state->completions.load(std::memory_order_acquire) == k_total);
  assert(state->stopped.load(std::memory_order_acquire) == 0);
  assert(state->errors.load(std::memory_order_acquire) == 0);
}

}  // namespace

int main() {
  test_multithreaded_post_stress();
  return 0;
}
