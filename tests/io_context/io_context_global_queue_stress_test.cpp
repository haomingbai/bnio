#include <bupp/base/linux/submission_queue_entry.h>

#include <atomic>
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
};

struct queued_nop_operation : public bupp::io_context::operation_base {
  explicit queued_nop_operation(queued_nop_state& state) noexcept
      : state(&state) {}

  [[nodiscard]] int prepare_for_submit() noexcept override { return 0; }

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

    const unsigned completed =
        state->completions.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (completed == state->target && state->context != nullptr) {
      state->completion_cv.notify_all();
      (void)state->context->stop();
    }
  }

  queued_nop_state* state = nullptr;
  bool submit_error = false;
};

void wait_for_run_threads(
    bupp::io_context::post_scheduler scheduler, thread_recorder& recorder,
    std::vector<std::unique_ptr<post_record_operation>>& operations,
    std::size_t expected_threads) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);

  while (recorder.unique_thread_count() < expected_threads &&
         std::chrono::steady_clock::now() < deadline) {
    for (std::size_t index = 0; index < expected_threads; ++index) {
      operations.emplace_back(
          std::make_unique<post_record_operation>(recorder));
      scheduler.post(*operations.back());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  assert(recorder.unique_thread_count() == expected_threads);
}

enum class flush_strategy {
  timer,
  manual_once,
  concurrent_manual,
};

void enqueue_operation_range(
    bupp::io_context::post_scheduler scheduler,
    const std::vector<std::unique_ptr<queued_nop_operation>>& operations,
    unsigned first, unsigned last) {
  for (unsigned index = first; index < last; ++index) {
    scheduler.enqueue_io(*operations[index]);
  }
}

void run_global_queue_nop_batch(unsigned worker_count, unsigned producer_count,
                                unsigned operation_count,
                                flush_strategy strategy) {
  bupp::io_context_options options;
  options.concurrency_hint = worker_count;
  options.platform.max_queued_io_operations = 0;
  options.platform.queued_io_flush_after = strategy == flush_strategy::timer
                                               ? std::chrono::milliseconds(1)
                                               : std::chrono::seconds(30);

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

  std::atomic_bool producers_done{false};
  std::vector<std::thread> flushers;
  if (strategy == flush_strategy::concurrent_manual) {
    flushers.reserve(worker_count);
    for (unsigned index = 0; index < worker_count; ++index) {
      flushers.emplace_back([scheduler, &producers_done] {
        while (!producers_done.load(std::memory_order_acquire) ||
               scheduler.queued_io_size() != 0) {
          const std::error_code flush_error = scheduler.flush_io_queue();
          assert(!flush_error);
          std::this_thread::yield();
        }
      });
    }
  }

  for (std::thread& producer : producers) {
    producer.join();
  }
  producers_done.store(true, std::memory_order_release);

  if (strategy == flush_strategy::manual_once) {
    assert(scheduler.queued_io_size() == operation_count);
    const std::error_code flush_error = scheduler.flush_io_queue();
    assert(!flush_error);
  }

  for (std::thread& flusher : flushers) {
    flusher.join();
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
  assert(scheduler.queued_io_size() == 0);
}

void test_global_queue_balances_queued_io() {
  run_global_queue_nop_batch(4, 1, 128, flush_strategy::timer);
}

void test_global_queue_pressure_many_queued_io_operations() {
  run_global_queue_nop_batch(4, 1, 4096, flush_strategy::timer);
}

void test_global_queue_multi_producer_timer_flush() {
  run_global_queue_nop_batch(4, 8, 2048, flush_strategy::timer);
}

void test_global_queue_multi_producer_manual_flush() {
  run_global_queue_nop_batch(4, 8, 2048, flush_strategy::manual_once);
}

void test_global_queue_concurrent_manual_flush_pressure() {
  run_global_queue_nop_batch(4, 8, 4096, flush_strategy::concurrent_manual);
}

}  // namespace

int main() {
  test_global_queue_balances_queued_io();
  test_global_queue_pressure_many_queued_io_operations();
  test_global_queue_multi_producer_timer_flush();
  test_global_queue_multi_producer_manual_flush();
  test_global_queue_concurrent_manual_flush_pressure();
  return 0;
}
