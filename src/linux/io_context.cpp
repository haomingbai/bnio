#include <bupp/linux/io_context.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
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

namespace detail {

timer_operation_base::timer_operation_base(io_context& context) noexcept
    : timer_context_(&context) {}

}  // namespace detail

io_context::timer_wakeup_operation::timer_wakeup_operation(
    io_context& context) noexcept
    : context_(&context) {}

void io_context::timer_wakeup_operation::set_timeout(
    duration timeout) noexcept {
  timeout_.reset(timeout);
}

void io_context::timer_wakeup_operation::prepare(
    base::submission_queue_entry& sqe) noexcept {
  timeout_.prepare_timeout(sqe, 0, 0);
}

void io_context::timer_wakeup_operation::execute() noexcept {
  context_->on_timer_wakeup();
}

io_context::timer_update_operation::timer_update_operation(
    io_context& context) noexcept
    : context_(&context) {}

void io_context::timer_update_operation::set_timeout(
    duration timeout) noexcept {
  timeout_.reset(timeout);
}

void io_context::timer_update_operation::prepare(
    base::submission_queue_entry& sqe) noexcept {
  timeout_.prepare_timeout_update(
      sqe,
      static_cast<std::uint64_t>(
          reinterpret_cast<std::uintptr_t>(&context_->timer_wakeup_operation_)),
      0);
}

void io_context::timer_update_operation::execute() noexcept {
  context_->on_timer_update();
}

io_context::timer_driver_operation::timer_driver_operation(
    io_context& context) noexcept
    : context_(&context) {}

void io_context::timer_driver_operation::execute() noexcept {
  context_->on_timer_driver();
}

io_context::queued_io_flush_operation::queued_io_flush_operation(
    io_context& context) noexcept
    : timer_operation_base(context) {}

void io_context::queued_io_flush_operation::execute() noexcept {
  timer_context_->on_queued_io_flush(timer_completion());
}

io_context::io_context() noexcept : io_context(io_context_options{}) {}

io_context::io_context(const io_context_options& options) noexcept
    : native_context_(options.platform.uring),
      linux_options_(options.platform),
      timer_wakeup_operation_(*this),
      timer_update_operation_(*this),
      timer_driver_operation_(*this),
      queued_io_flush_timer_(*this) {
  timers_.queued_io_flush_wait.emplace(*this);
}

io_context::~io_context() noexcept {
  std::lock_guard context_lock(timers_.mutex);
  for (auto& entry : timers_.timers) {
    detail::timer_slot* timer = entry.second;
    if (timer == nullptr) {
      continue;
    }
    std::lock_guard timer_lock(timer->mutex);
    timer->context = nullptr;
    timer->waiting_head = nullptr;
    timer->submitted_head.store(nullptr, std::memory_order_release);
  }
  timers_.timers.clear();
  timers_.heap.clear();
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
  if (linux_options_.queued_io_flush_after > duration::zero()) {
    (void)queued_io_flush_timer_.cancel();
  }
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

  const time_point deadline =
      clock::now() + linux_options_.queued_io_flush_after;
  bool should_post_driver = false;
  {
    std::lock_guard context_lock(timers_.mutex);
    detail::timer_slot& timer = queued_io_flush_timer_.timer_;
    std::lock_guard timer_lock(timer.mutex);
    if (timer.context != this || !timers_.queued_io_flush_wait.has_value() ||
        !timers_.queue_flush_wait()) {
      return;
    }

    timer.expiry = deadline;
    ++timer.generation;
    push_timer_operation(timer.submitted_head, *timers_.queued_io_flush_wait);
    if (timers_.queue_driver()) {
      should_post_driver = true;
    }
  }

  if (should_post_driver) {
    (void)native_context_.post(timer_driver_operation_);
  }
}

void io_context::register_timer(detail::timer_slot& timer) noexcept {
  std::lock_guard context_lock(timers_.mutex);
  std::lock_guard timer_lock(timer.mutex);
  if (timer.context != nullptr) {
    return;
  }

  timer.id = timers_.next_timer_id++;
  if (timer.id == 0) {
    timer.id = timers_.next_timer_id++;
  }
  timer.context = this;
  timer.generation = 0;
  timer.waiting_head = nullptr;
  timer.submitted_head.store(nullptr, std::memory_order_release);
  timers_.timers.emplace(timer.id, &timer);
}

void io_context::unregister_timer(detail::timer_slot& timer) noexcept {
  detail::timer_operation_base* canceled = nullptr;
  {
    std::lock_guard context_lock(timers_.mutex);
    std::lock_guard timer_lock(timer.mutex);
    if (timer.context != this) {
      return;
    }

    auto iterator = timers_.timers.find(timer.id);
    if (iterator != timers_.timers.end()) {
      detail::timer_slot* mapped_timer = iterator->second;
      if (mapped_timer != nullptr && mapped_timer == &timer) {
        timers_.timers.erase(iterator);
      }
    }

    ++timer.generation;
    timer.context = nullptr;
    canceled = take_timer_waiters_locked(timer);
  }

  post_timer_operations(canceled, detail::timer_completion_kind::stopped);
}

std::size_t io_context::cancel_timer(detail::timer_slot& timer) noexcept {
  detail::timer_operation_base* canceled = nullptr;
  std::size_t count = 0;
  {
    std::lock_guard context_lock(timers_.mutex);
    std::lock_guard timer_lock(timer.mutex);
    if (timer.context != this) {
      return 0;
    }

    ++timer.generation;
    canceled = take_timer_waiters_locked(timer);
    count = count_timer_operations(canceled);
    schedule_timer_wakeup_locked();
  }

  post_timer_operations(canceled, detail::timer_completion_kind::stopped);
  return count;
}

std::size_t io_context::set_timer_expiry(detail::timer_slot& timer,
                                         time_point expiry) noexcept {
  detail::timer_operation_base* canceled = nullptr;
  std::size_t count = 0;
  {
    std::lock_guard context_lock(timers_.mutex);
    std::lock_guard timer_lock(timer.mutex);
    if (timer.context != this) {
      return 0;
    }

    timer.expiry = expiry;
    ++timer.generation;
    canceled = take_timer_waiters_locked(timer);
    count = count_timer_operations(canceled);
    schedule_timer_wakeup_locked();
  }

  post_timer_operations(canceled, detail::timer_completion_kind::stopped);
  return count;
}

io_context::time_point io_context::timer_expiry(
    const detail::timer_slot& timer) const noexcept {
  std::lock_guard context_lock(timers_.mutex);
  std::lock_guard timer_lock(timer.mutex);
  return timer.expiry;
}

void io_context::start_timer_wait(detail::timer_operation_base& operation,
                                  detail::timer_slot& timer) noexcept {
  push_timer_operation(timer.submitted_head, operation);
  post_timer_driver();
}

void io_context::on_timer_wakeup() noexcept {
  {
    std::lock_guard context_lock(timers_.mutex);
    timers_.complete_wakeup();
  }
  post_timer_driver();
}

void io_context::on_timer_update() noexcept {
  std::lock_guard context_lock(timers_.mutex);
  timers_.complete_update();
  schedule_timer_wakeup_locked();
}

void io_context::on_timer_driver() noexcept {
  detail::timer_operation_base* ready = nullptr;

  {
    std::lock_guard context_lock(timers_.mutex);
    timers_.complete_driver();

    for (auto& entry : timers_.timers) {
      detail::timer_slot* timer = entry.second;
      if (timer == nullptr) {
        continue;
      }
      (void)drain_timer_submissions_locked(*timer);
    }

    const time_point now = clock::now();
    while (!timers_.heap.empty() && timers_.heap.front().deadline <= now) {
      const detail::timer_heap_item item = timers_.heap.front();
      timers_.pop_heap();

      auto iterator = timers_.timers.find(item.timer_id);
      if (iterator == timers_.timers.end() || iterator->second == nullptr) {
        continue;
      }

      detail::timer_slot* timer = iterator->second;
      std::lock_guard timer_lock(timer->mutex);
      if (timer->context != this || timer->generation != item.generation) {
        continue;
      }

      detail::timer_operation_base* operations = timer->waiting_head;
      timer->waiting_head = nullptr;
      if (operations != nullptr) {
        operations = reverse_timer_operations(operations);
        detail::timer_operation_base* tail = operations;
        while (tail->timer_next_ != nullptr) {
          tail = tail->timer_next_;
        }
        tail->timer_next_ = ready;
        ready = operations;
      }
    }

    schedule_timer_wakeup_locked();
  }

  post_timer_operations(ready, detail::timer_completion_kind::value);
}

void io_context::on_queued_io_flush(
    detail::timer_completion_kind completion) noexcept {
  {
    std::lock_guard context_lock(timers_.mutex);
    timers_.complete_flush_wait();
  }

  if (completion == detail::timer_completion_kind::value) {
    (void)flush_io_queue();
    return;
  }

  if (queued_io_size() != 0) {
    arm_flush_timer();
  }
}

void io_context::post_timer_driver() noexcept {
  bool should_post = false;
  {
    std::lock_guard context_lock(timers_.mutex);
    if (timers_.queue_driver()) {
      should_post = true;
    }
  }

  if (should_post) {
    (void)native_context_.post(timer_driver_operation_);
  }
}

std::size_t io_context::drain_timer_submissions_locked(
    detail::timer_slot& timer) noexcept {
  std::lock_guard timer_lock(timer.mutex);
  if (timer.context != this) {
    return 0;
  }

  detail::timer_operation_base* submitted =
      timer.submitted_head.exchange(nullptr, std::memory_order_acq_rel);
  if (submitted == nullptr) {
    return 0;
  }

  submitted = reverse_timer_operations(submitted);
  const std::size_t count = count_timer_operations(submitted);

  detail::timer_operation_base* tail = submitted;
  while (tail->timer_next_ != nullptr) {
    tail = tail->timer_next_;
  }
  tail->timer_next_ = timer.waiting_head;
  timer.waiting_head = submitted;
  timers_.push_heap(detail::timer_heap_item{
      .deadline = timer.expiry,
      .timer_id = timer.id,
      .generation = timer.generation,
  });

  return count;
}

detail::timer_operation_base* io_context::take_timer_waiters_locked(
    detail::timer_slot& timer) noexcept {
  detail::timer_operation_base* submitted =
      timer.submitted_head.exchange(nullptr, std::memory_order_acq_rel);
  detail::timer_operation_base* waiting = timer.waiting_head;
  timer.waiting_head = nullptr;

  if (submitted == nullptr) {
    return waiting;
  }

  submitted = reverse_timer_operations(submitted);
  detail::timer_operation_base* tail = submitted;
  while (tail->timer_next_ != nullptr) {
    tail = tail->timer_next_;
  }
  tail->timer_next_ = waiting;
  return submitted;
}

void io_context::push_timer_operation(
    std::atomic<detail::timer_operation_base*>& head,
    detail::timer_operation_base& operation) noexcept {
  detail::timer_operation_base* current_head =
      head.load(std::memory_order_acquire);
  do {
    operation.timer_next_ = current_head;
  } while (!head.compare_exchange_weak(current_head, &operation,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire));
}

detail::timer_operation_base* io_context::reverse_timer_operations(
    detail::timer_operation_base* operations) noexcept {
  detail::timer_operation_base* reversed = nullptr;
  while (operations != nullptr) {
    detail::timer_operation_base* operation = operations;
    operations = operations->timer_next_;
    operation->timer_next_ = reversed;
    reversed = operation;
  }
  return reversed;
}

std::size_t io_context::count_timer_operations(
    detail::timer_operation_base* operations) noexcept {
  std::size_t count = 0;
  while (operations != nullptr) {
    ++count;
    operations = operations->timer_next_;
  }
  return count;
}

void io_context::post_timer_operations(
    detail::timer_operation_base* operations,
    detail::timer_completion_kind completion) noexcept {
  while (operations != nullptr) {
    detail::timer_operation_base* operation = operations;
    operations = operations->timer_next_;
    operation->timer_next_ = nullptr;
    operation->timer_completion_ = completion;
    (void)native_context_.post(*operation);
  }
}

bool io_context::timer_state_data::queue_driver() noexcept {
  if (driver == queued_operation_state::posted) {
    return false;
  }
  driver = queued_operation_state::posted;
  return true;
}

void io_context::timer_state_data::complete_driver() noexcept {
  driver = queued_operation_state::idle;
}

bool io_context::timer_state_data::queue_flush_wait() noexcept {
  if (queued_io_flush == queued_operation_state::posted) {
    return false;
  }
  queued_io_flush = queued_operation_state::posted;
  return true;
}

void io_context::timer_state_data::complete_flush_wait() noexcept {
  queued_io_flush = queued_operation_state::idle;
}

bool io_context::timer_state_data::can_submit_wakeup() const noexcept {
  return timeout == timeout_state::idle;
}

bool io_context::timer_state_data::can_submit_update(
    time_point deadline) const noexcept {
  return timeout == timeout_state::armed && deadline != armed_deadline;
}

void io_context::timer_state_data::complete_wakeup() noexcept {
  timeout = timeout == timeout_state::updating ? timeout_state::update_pending
                                               : timeout_state::idle;
}

void io_context::timer_state_data::complete_update() noexcept {
  if (timeout == timeout_state::updating) {
    timeout = timeout_state::armed;
  } else if (timeout == timeout_state::update_pending) {
    timeout = timeout_state::idle;
  }
}

void io_context::timer_state_data::mark_wakeup_submitted(
    time_point deadline) noexcept {
  timeout = timeout_state::armed;
  armed_deadline = deadline;
}

void io_context::timer_state_data::mark_update_submitted(
    time_point deadline) noexcept {
  timeout = timeout_state::updating;
  armed_deadline = deadline;
}

void io_context::timer_state_data::push_heap(
    detail::timer_heap_item item) noexcept {
  heap.push_back(item);
  sift_heap_up(heap.size() - 1);
}

void io_context::timer_state_data::pop_heap() noexcept {
  if (heap.empty()) {
    return;
  }

  const std::size_t last = heap.size() - 1;
  swap_heap_items(0, last);
  heap.pop_back();
  if (!heap.empty()) {
    sift_heap_down(0);
  }
}

void io_context::timer_state_data::swap_heap_items(
    std::size_t first, std::size_t second) noexcept {
  if (first == second) {
    return;
  }
  std::swap(heap[first], heap[second]);
}

void io_context::timer_state_data::sift_heap_up(std::size_t index) noexcept {
  while (index != 0) {
    const std::size_t parent = (index - 1) / 2;
    if (!heap_item_less(index, parent)) {
      break;
    }
    swap_heap_items(index, parent);
    index = parent;
  }
}

void io_context::timer_state_data::sift_heap_down(std::size_t index) noexcept {
  for (;;) {
    const std::size_t left = index * 2 + 1;
    const std::size_t right = left + 1;
    std::size_t smallest = index;

    if (left < heap.size() && heap_item_less(left, smallest)) {
      smallest = left;
    }
    if (right < heap.size() && heap_item_less(right, smallest)) {
      smallest = right;
    }
    if (smallest == index) {
      break;
    }
    swap_heap_items(index, smallest);
    index = smallest;
  }
}

bool io_context::timer_state_data::heap_item_less(
    std::size_t first, std::size_t second) const noexcept {
  const detail::timer_heap_item& left = heap[first];
  const detail::timer_heap_item& right = heap[second];
  if (left.deadline != right.deadline) {
    return left.deadline < right.deadline;
  }
  return left.timer_id < right.timer_id;
}

void io_context::schedule_timer_wakeup_locked() noexcept {
  if (timers_.heap.empty()) {
    return;
  }

  const time_point deadline = timers_.heap.front().deadline;
  if (timers_.can_submit_wakeup()) {
    submit_timer_wakeup_locked(deadline);
    return;
  }

  if (timers_.can_submit_update(deadline)) {
    submit_timer_update_locked(deadline);
  }
}

void io_context::submit_timer_wakeup_locked(time_point deadline) noexcept {
  if (!timers_.can_submit_wakeup()) {
    return;
  }

  timer_wakeup_operation_.set_timeout(
      std::max(deadline - clock::now(), duration::zero()));

  int result = native_context_.submit(timer_wakeup_operation_);
  if (result == -EAGAIN) {
    (void)native_context_.submit();
    result = native_context_.submit(timer_wakeup_operation_);
  }

  if (result >= 0) {
    timers_.mark_wakeup_submitted(deadline);
  }
}

void io_context::submit_timer_update_locked(time_point deadline) noexcept {
  if (!timers_.can_submit_update(deadline)) {
    return;
  }

  timer_update_operation_.set_timeout(
      std::max(deadline - clock::now(), duration::zero()));

  int result = native_context_.submit(timer_update_operation_);
  if (result == -EAGAIN) {
    (void)native_context_.submit();
    result = native_context_.submit(timer_update_operation_);
  }

  if (result >= 0) {
    timers_.mark_update_submitted(deadline);
  }
}

steady_timer::steady_timer(io_context& context) noexcept {
  timer_.expiry = clock::now();
  context.register_timer(timer_);
}

steady_timer::steady_timer(io_context& context, time_point expiry) noexcept {
  timer_.expiry = expiry;
  context.register_timer(timer_);
}

steady_timer::~steady_timer() noexcept {
  if (timer_.context != nullptr) {
    timer_.context->unregister_timer(timer_);
  }
}

steady_timer::steady_timer(steady_timer&& other) noexcept {
  io_context* context = other.timer_.context;
  time_point expiry = clock::now();
  if (context != nullptr) {
    expiry = context->timer_expiry(other.timer_);
    context->unregister_timer(other.timer_);
    timer_.expiry = expiry;
    context->register_timer(timer_);
  }
}

steady_timer& steady_timer::operator=(steady_timer&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  if (timer_.context != nullptr) {
    timer_.context->unregister_timer(timer_);
  }

  io_context* context = other.timer_.context;
  if (context != nullptr) {
    const time_point expiry = context->timer_expiry(other.timer_);
    context->unregister_timer(other.timer_);
    timer_.expiry = expiry;
    context->register_timer(timer_);
  }
  return *this;
}

steady_timer::time_point steady_timer::expiry() const noexcept {
  return timer_.context->timer_expiry(timer_);
}

std::size_t steady_timer::expires_at(time_point expiry) noexcept {
  return timer_.context->set_timer_expiry(timer_, expiry);
}

std::size_t steady_timer::cancel() noexcept {
  return timer_.context->cancel_timer(timer_);
}

}  // namespace bupp
