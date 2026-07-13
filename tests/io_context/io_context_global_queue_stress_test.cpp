#include <bupp/base/linux/submission_queue_entry.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <thread>

#include "io_context_runtime_test_support.h"

namespace {

class thread_recorder {
 public:
  void record_current_thread() noexcept {
    std::lock_guard lock(mutex_);
    threads_.insert(std::this_thread::get_id());
  }

  [[nodiscard]] std::size_t unique_thread_count() const noexcept {
    std::lock_guard lock(mutex_);
    return threads_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::set<std::thread::id> threads_;
};

struct post_record_operation
    : public bupp::async_io::linux_native::io_uring_operation_base {
  explicit post_record_operation(thread_recorder& recorder) noexcept
      : recorder(&recorder) {}

  void execute() noexcept override { recorder->record_current_thread(); }

  thread_recorder* recorder = nullptr;
};

struct queued_nop_state {
  bupp::io_context* context = nullptr;
  unsigned target = 0;
  std::atomic<unsigned> completions{0};
  std::atomic<unsigned> errors{0};
  mutable std::mutex mutex;
  std::condition_variable completion_cv;
  thread_recorder recorder;

  [[nodiscard]] bool wait_for_target(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex);
    return completion_cv.wait_for(lock, timeout, [this] {
      return completions.load(std::memory_order_acquire) == target;
    });
  }

  [[nodiscard]] bool wait_for_count(unsigned count,
                                    std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex);
    return completion_cv.wait_for(lock, timeout, [this, count] {
      return completions.load(std::memory_order_acquire) >= count;
    });
  }
};

struct queued_nop_operation : public bupp::io_context::operation_base {
  explicit queued_nop_operation(queued_nop_state& state) noexcept
      : state(&state) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_nop();
  }

  void complete_submit_error(int result) noexcept override {
    this->result = result;
    submit_error = true;
  }

  void execute() noexcept override {
    state->recorder.record_current_thread();
    if (submit_error || this->result < 0) {
      state->errors.fetch_add(1, std::memory_order_relaxed);
    }

    unsigned completed = 0;
    {
      std::lock_guard lock(state->mutex);
      completed =
          state->completions.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    state->completion_cv.notify_all();
    if (completed == state->target && state->context != nullptr) {
      (void)state->context->stop();
    }
  }

  queued_nop_state* state = nullptr;
  bool submit_error = false;
};

void wait_for_run_threads(
    bupp::io_context::post_scheduler scheduler, thread_recorder& recorder,
    std::vector<std::unique_ptr<post_record_operation>>& operations,
    std::size_t post_batch_size) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);

  while (recorder.unique_thread_count() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    for (std::size_t index = 0; index < post_batch_size; ++index) {
      operations.emplace_back(
          std::make_unique<post_record_operation>(recorder));
      scheduler.post(*operations.back());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  assert(recorder.unique_thread_count() != 0);
}

void enqueue_operation_range(
    bupp::io_context::post_scheduler scheduler,
    const std::vector<std::unique_ptr<queued_nop_operation>>& operations,
    unsigned first, unsigned last) {
  for (unsigned index = first; index < last; ++index) {
    scheduler.enqueue_io(*operations[index]);
  }
}

void run_global_queue_nop_batch(unsigned worker_count, unsigned producer_count,
                                unsigned operation_count) {
  bupp::io_context_options options;
  options.concurrency_hint = worker_count;
  bupp::io_context context(options);
  if (!context_available(context)) {
    return;
  }

  auto scheduler = context.get_post_scheduler();
  std::vector<std::thread> threads;
  threads.reserve(worker_count);
  for (unsigned index = 0; index < worker_count; ++index) {
    threads.emplace_back([&context] { context.run(); });
  }

  thread_recorder run_thread_recorder;
  std::vector<std::unique_ptr<post_record_operation>> warmup_operations;
  wait_for_run_threads(scheduler, run_thread_recorder, warmup_operations,
                       worker_count);

  queued_nop_state state;
  state.context = &context;
  state.target = operation_count;

  std::vector<std::unique_ptr<queued_nop_operation>> operations;
  operations.reserve(operation_count);
  for (unsigned index = 0; index < operation_count; ++index) {
    operations.emplace_back(std::make_unique<queued_nop_operation>(state));
  }

  std::vector<std::thread> producers;
  producers.reserve(producer_count);
  for (unsigned producer = 0; producer < producer_count; ++producer) {
    const unsigned first = static_cast<unsigned>(
        (static_cast<std::uint64_t>(operation_count) * producer) /
        producer_count);
    const unsigned last = static_cast<unsigned>(
        (static_cast<std::uint64_t>(operation_count) * (producer + 1)) /
        producer_count);
    producers.emplace_back([scheduler, &operations, first, last] {
      enqueue_operation_range(scheduler, operations, first, last);
    });
  }

  for (std::thread& producer : producers) {
    producer.join();
  }
  const bool completed = state.wait_for_target(std::chrono::seconds(5));
  if (!completed) {
    (void)context.stop();
  }

  for (std::thread& thread : threads) {
    thread.join();
  }

  assert(completed);
  assert(state.completions.load(std::memory_order_acquire) == operation_count);
  assert(state.errors.load(std::memory_order_acquire) == 0);
  assert(state.recorder.unique_thread_count() >= 1);
  assert(state.recorder.unique_thread_count() <= worker_count);
}

void test_global_queue_balances_io() { run_global_queue_nop_batch(4, 1, 128); }

void test_global_queue_pressure_many_io_operations() {
  run_global_queue_nop_batch(4, 1, 4096);
}

void test_global_queue_multi_producer_passive_drain() {
  run_global_queue_nop_batch(4, 8, 4096);
}

void test_idle_worker_handoff_does_not_lose_wakeups() {
  constexpr unsigned worker_count = 4;
  constexpr unsigned operation_count = 512;

  bupp::io_context_options options;
  options.concurrency_hint = worker_count;
  bupp::io_context context(options);
  if (!context_available(context)) {
    return;
  }

  std::vector<std::thread> workers;
  for (unsigned index = 0; index < worker_count; ++index) {
    workers.emplace_back([&context] { context.run(); });
  }

  queued_nop_state state;
  state.context = &context;
  state.target = operation_count;
  std::vector<std::unique_ptr<queued_nop_operation>> operations;
  operations.reserve(operation_count);
  for (unsigned index = 0; index < operation_count; ++index) {
    operations.emplace_back(std::make_unique<queued_nop_operation>(state));
  }

  auto scheduler = context.get_post_scheduler();
  for (unsigned index = 0; index < operation_count; ++index) {
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    scheduler.enqueue_io(*operations[index]);
    assert(state.wait_for_count(index + 1, std::chrono::seconds(1)));
  }

  for (std::thread& worker : workers) {
    worker.join();
  }
  assert(state.errors.load(std::memory_order_acquire) == 0);
}

}  // namespace

int main() {
  test_global_queue_balances_io();
  test_global_queue_pressure_many_io_operations();
  test_global_queue_multi_producer_passive_drain();
  test_idle_worker_handoff_does_not_lose_wakeups();
  return 0;
}
