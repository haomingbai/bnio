#include <bupp/linux/io_context.h>

#include <algorithm>
#include <utility>

namespace bupp {

namespace {

[[nodiscard]] io_context::operation_base* reverse_operations(
    io_context::operation_base* operations) noexcept {
  io_context::operation_base* reversed = nullptr;
  while (operations != nullptr) {
    io_context::operation_base* operation = operations;
    operations = operations->pending_next;
    operation->pending_next = reversed;
    reversed = operation;
  }
  return reversed;
}

[[nodiscard]] std::error_code make_submit_error(int result) noexcept {
  return std::error_code(-result, std::generic_category());
}

}  // namespace

io_context::io_context() noexcept : io_context(io_context_options{}) {}

io_context::io_context(const io_context_options& options) noexcept
    : native_context_(options.platform.uring),
      linux_options_(options.platform),
      timer_deadline_(clock::now()) {
  if (linux_options_.queued_io_flush_after > duration::zero()) {
    timer_thread_ = std::thread([this] { timer_loop(); });
  }
}

io_context::~io_context() noexcept {
  {
    std::lock_guard lock(timer_mutex_);
    timer_stop_ = true;
    timer_armed_ = false;
    ++timer_generation_;
  }
  timer_cv_.notify_all();
  if (timer_thread_.joinable()) {
    timer_thread_.join();
  }
}

bool io_context::is_open() const noexcept { return native_context_.is_open(); }

void io_context::run() noexcept { native_context_.run(); }

int io_context::stop() noexcept { return native_context_.stop(); }

bool io_context::is_in_context() const noexcept {
  return native_context_.is_in_context();
}

std::size_t io_context::queued_io_size() const noexcept {
  std::lock_guard lock(queue_mutex_);
  return pending_io_count_;
}

void io_context::enqueue_io(operation_base& operation) noexcept {
  bool should_arm_timer = false;
  bool should_flush = false;
  {
    std::lock_guard lock(queue_mutex_);
    operation.pending_next = pending_io_head_;
    pending_io_head_ = &operation;
    ++pending_io_count_;

    should_arm_timer = pending_io_count_ == 1;
    should_flush = linux_options_.max_queued_io_operations == 0 ||
                   pending_io_count_ >= linux_options_.max_queued_io_operations;
  }

  if (should_arm_timer) {
    arm_flush_timer();
  }
  if (should_flush) {
    (void)flush_io_queue();
  }
}

void io_context::submit_direct(operation_base& operation) noexcept {
  const int result = native_context_.submit(operation);
  if (result < 0) {
    operation.complete_submit_error(result);
    (void)native_context_.post(operation);
  }
}

void io_context::post(operation_base& operation) noexcept {
  (void)native_context_.post(operation);
}

std::error_code io_context::flush_io_queue() noexcept {
  operation_base* operations = take_pending_io();
  if (operations == nullptr) {
    return {};
  }
  cancel_flush_timer();
  return flush_operations(operations);
}

std::error_code io_context::flush_operations(
    operation_base* operations) noexcept {
  int first_error = 0;
  operation_base* failed_head = nullptr;
  operation_base* failed_tail = nullptr;

  auto append_failed = [&](operation_base& operation) noexcept {
    operation.pending_next = nullptr;
    if (failed_tail == nullptr) {
      failed_head = &operation;
    } else {
      failed_tail->pending_next = &operation;
    }
    failed_tail = &operation;
  };

  native_context_.submit_batch([&](auto& prepare, auto& submit) noexcept {
    std::size_t prepared_count = 0;

    for (operation_base* operation = operations; operation != nullptr;) {
      operation_base* next = operation->pending_next;
      operation->pending_next = nullptr;

      int prepare_result = prepare(*operation);
      if (prepare_result == -EAGAIN && prepared_count != 0) {
        const int submit_result = submit();
        if (submit_result < 0 && first_error == 0) {
          first_error = submit_result;
        }
        prepared_count = 0;
        prepare_result = prepare(*operation);
      }

      if (prepare_result < 0) {
        operation->complete_submit_error(prepare_result);
        append_failed(*operation);
        if (first_error == 0) {
          first_error = prepare_result;
        }
      } else {
        ++prepared_count;
      }

      operation = next;
    }

    if (prepared_count != 0) {
      const int submit_result = submit();
      if (submit_result < 0 && first_error == 0) {
        first_error = submit_result;
      }
    }
  });

  while (failed_head != nullptr) {
    operation_base* operation = failed_head;
    failed_head = failed_head->pending_next;
    operation->pending_next = nullptr;
    (void)native_context_.post(*operation);
  }

  if (first_error < 0) {
    return make_submit_error(first_error);
  }
  return {};
}

io_context::operation_base* io_context::take_pending_io() noexcept {
  std::lock_guard lock(queue_mutex_);
  operation_base* operations = reverse_operations(pending_io_head_);
  pending_io_head_ = nullptr;
  pending_io_count_ = 0;
  return operations;
}

void io_context::arm_flush_timer() noexcept {
  if (linux_options_.queued_io_flush_after <= duration::zero()) {
    (void)flush_io_queue();
    return;
  }

  {
    std::lock_guard lock(timer_mutex_);
    if (timer_stop_) {
      return;
    }
    timer_armed_ = true;
    timer_deadline_ = clock::now() + linux_options_.queued_io_flush_after;
    ++timer_generation_;
  }
  timer_cv_.notify_all();
}

void io_context::cancel_flush_timer() noexcept {
  if (linux_options_.queued_io_flush_after <= duration::zero()) {
    return;
  }

  {
    std::lock_guard lock(timer_mutex_);
    timer_armed_ = false;
    ++timer_generation_;
  }
  timer_cv_.notify_all();
}

void io_context::timer_loop() noexcept {
  std::unique_lock lock(timer_mutex_);
  for (;;) {
    timer_cv_.wait(lock, [this] { return timer_stop_ || timer_armed_; });
    if (timer_stop_) {
      return;
    }

    const std::uint64_t generation = timer_generation_;
    const time_point deadline = timer_deadline_;
    const bool changed =
        timer_cv_.wait_until(lock, deadline, [this, generation] {
          return timer_stop_ || !timer_armed_ ||
                 timer_generation_ != generation;
        });

    if (timer_stop_) {
      return;
    }
    if (changed) {
      continue;
    }

    timer_armed_ = false;
    ++timer_generation_;
    lock.unlock();
    (void)flush_io_queue();
    lock.lock();
  }
}

}  // namespace bupp
