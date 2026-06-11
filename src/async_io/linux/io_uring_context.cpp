#include <bupp/async_io/linux/io_uring_context.h>

#include <cerrno>

namespace bupp::async_io::linux_native {

namespace {

io_uring_operation_base* reverse_tasks(
    io_uring_operation_base* tasks) noexcept {
  io_uring_operation_base* reversed = nullptr;
  while (tasks != nullptr) {
    io_uring_operation_base* next = tasks->next;
    tasks->next = reversed;
    reversed = tasks;
    tasks = next;
  }
  return reversed;
}

void execute_tasks(io_uring_operation_base* tasks) noexcept {
  while (tasks != nullptr) {
    io_uring_operation_base* operation = tasks;
    tasks = tasks->next;
    operation->next = nullptr;
    operation->execute();
  }
}

}  // namespace

thread_local io_uring_context* io_uring_context::current_context_ = nullptr;
thread_local io_uring_context::operation_queue*
    io_uring_context::current_local_tasks_ = nullptr;

struct io_uring_context::cqe_data {
  void* user_data = nullptr;
  int result = 0;
  unsigned flags = 0;
};

struct io_uring_context::operation_queue {
  void push(io_uring_operation_base& operation) noexcept;
  void push(io_uring_operation_base* operations) noexcept;
  [[nodiscard]] io_uring_operation_base* pop_all() noexcept;

  io_uring_operation_base* head = nullptr;
};

void io_uring_context::operation_queue::push(
    io_uring_operation_base& operation) noexcept {
  operation.next = head;
  head = &operation;
}

void io_uring_context::operation_queue::push(
    io_uring_operation_base* operations) noexcept {
  while (operations != nullptr) {
    io_uring_operation_base* operation = operations;
    operations = operations->next;
    operation->next = nullptr;
    push(*operation);
  }
}

io_uring_operation_base* io_uring_context::operation_queue::pop_all() noexcept {
  io_uring_operation_base* operations = head;
  head = nullptr;
  return operations;
}

io_uring_context::io_uring_context() noexcept = default;

io_uring_context::io_uring_context(
    const io_uring_context_options& options) noexcept {
  (void)queue_init(options);
}

io_uring_context::~io_uring_context() noexcept { queue_exit(); }

int io_uring_context::queue_init(
    const io_uring_context_options& options) noexcept {
  global_tasks_.store(nullptr, std::memory_order_release);

  io_waiter_active_.store(false, std::memory_order_release);

  std::lock_guard lock(uring_mutex_);
  if (queue_initialized_) {
    return -EALREADY;
  }
  queue_initialized_ = true;
  cqe_batch_window_ =
      options.cqe_batch_window == 0 ? 1 : options.cqe_batch_window;
  wait_spin_count_ = options.wait_spin_count;
  cqe_inline_completion_threshold_ = options.cqe_inline_completion_threshold;
  wake_task_pending_ = false;
  const int result = ring_.queue_init(options.entries, options.setup_flags);
  state_.store(result >= 0 ? context_state::running : context_state::finished,
               std::memory_order_release);
  return result;
}

void io_uring_context::queue_exit() noexcept {
  state_.store(context_state::finished, std::memory_order_release);
  io_waiter_active_.store(false, std::memory_order_release);
  global_tasks_.store(nullptr, std::memory_order_release);
  notify_waiters();

  std::lock_guard lock(uring_mutex_);
  wake_task_pending_ = false;
  ring_.queue_exit();
}

bool io_uring_context::is_open() const noexcept {
  std::lock_guard lock(uring_mutex_);
  return ring_.is_open();
}

int io_uring_context::submit() noexcept {
  assert_running();

  int result = 0;
  {
    std::lock_guard lock(uring_mutex_);
    result = submit_locked();
  }

  if (result >= 0) {
    notify_waiters();
  }
  return result;
}

int io_uring_context::post(io_uring_operation_base& operation) noexcept {
  assert_running();

  if (current_context_ == this && current_local_tasks_ != nullptr) {
    current_local_tasks_->push(operation);
    return 0;
  }

  push_global_task(operation);

  if (io_waiter_active_.load(std::memory_order_acquire)) {
    return submit_wake_task();
  }
  return 0;
}

void io_uring_context::run() noexcept {
  assert_running();
  if (!is_open()) {
    return;
  }

  operation_queue local_tasks;
  io_uring_context* previous_context = current_context_;
  operation_queue* previous_local_tasks = current_local_tasks_;
  current_context_ = this;
  current_local_tasks_ = &local_tasks;

  run_phase phase = run_phase::run_ready_tasks;

  while (phase != run_phase::finished) {
    switch (phase) {
      case run_phase::run_ready_tasks:
        phase = handle_run_ready_tasks(local_tasks);
        break;

      case run_phase::wait_for_work:
        phase = handle_wait_for_work(local_tasks);
        break;

      case run_phase::finish_drain:
        phase = handle_finish_drain(local_tasks);
        break;

      case run_phase::finished:
        break;
    }
  }

  current_context_ = previous_context;
  current_local_tasks_ = previous_local_tasks;
}

int io_uring_context::stop() noexcept {
  context_state expected = context_state::running;
  if (!state_.compare_exchange_strong(expected, context_state::finishing,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire) &&
      expected != context_state::finishing) {
    return 0;
  }

  notify_waiters();

  if (io_waiter_active_.load(std::memory_order_acquire)) {
    return submit_wake_task();
  }
  return 0;
}

bool io_uring_context::is_in_context() const noexcept {
  return current_context_ == this;
}

int io_uring_context::submit_locked() noexcept {
  if (!ring_.is_open()) {
    return -EINVAL;
  }
  return ring_.submit();
}

void io_uring_context::assert_running() const noexcept {
#ifndef NDEBUG
  assert(state_.load(std::memory_order_acquire) == context_state::running);
#endif
}

io_uring_context::run_phase io_uring_context::handle_run_ready_tasks(
    operation_queue& local_tasks) noexcept {
  if (io_uring_operation_base* operations =
          reverse_tasks(local_tasks.pop_all())) {
    execute_tasks(operations);
    return run_phase::run_ready_tasks;
  }

  if (move_global_tasks(local_tasks)) {
    return run_phase::run_ready_tasks;
  }

  return should_finish() ? run_phase::finish_drain : run_phase::wait_for_work;
}

io_uring_context::run_phase io_uring_context::handle_wait_for_work(
    operation_queue& local_tasks) noexcept {
  const run_phase spin_result = spin_for_work(local_tasks);
  if (spin_result != run_phase::wait_for_work) {
    return spin_result;
  }

  if (!io_waiter_active_.exchange(true, std::memory_order_acq_rel)) {
    return wait_for_io_work(local_tasks);
  }

  return wait_for_condition_work(local_tasks);
}

io_uring_context::run_phase io_uring_context::handle_finish_drain(
    operation_queue& local_tasks) noexcept {
  finish(local_tasks);
  return run_phase::finished;
}

io_uring_context::run_phase io_uring_context::spin_for_work(
    operation_queue& local_tasks) noexcept {
  for (unsigned round = 0; round < wait_spin_count_; ++round) {
    if (collect_ready_cqes(local_tasks) || move_global_tasks(local_tasks)) {
      return run_phase::run_ready_tasks;
    }
    if (should_finish()) {
      return run_phase::finish_drain;
    }
  }

  return run_phase::wait_for_work;
}

io_uring_context::run_phase io_uring_context::wait_for_condition_work(
    operation_queue& local_tasks) noexcept {
  for (;;) {
    const run_phase spin_result = spin_for_work(local_tasks);
    if (spin_result != run_phase::wait_for_work) {
      return spin_result;
    }
    if (!io_waiter_active_.load(std::memory_order_acquire)) {
      return run_phase::wait_for_work;
    }

    std::unique_lock lock(wait_mutex_);
    wait_cv_.wait(lock, [this] {
      return should_finish() ||
             global_tasks_.load(std::memory_order_acquire) != nullptr ||
             !io_waiter_active_.load(std::memory_order_acquire);
    });
  }
}

io_uring_context::run_phase io_uring_context::wait_for_io_work(
    operation_queue& local_tasks) noexcept {
  if (collect_ready_cqes(local_tasks) || move_global_tasks(local_tasks) ||
      should_finish()) {
    io_waiter_active_.store(false, std::memory_order_release);
    notify_waiters();
    return should_finish() ? run_phase::finish_drain
                           : run_phase::run_ready_tasks;
  }

  const int wait_result = wait_for_cqe_event();
  io_waiter_active_.store(false, std::memory_order_release);
  notify_waiters();

  if (wait_result < 0 && !should_finish()) {
    return run_phase::finished;
  }

  if (collect_ready_cqes(local_tasks) || move_global_tasks(local_tasks)) {
    return run_phase::run_ready_tasks;
  }

  return should_finish() ? run_phase::finish_drain : run_phase::wait_for_work;
}

void io_uring_context::push_global_task(
    io_uring_operation_base& operation) noexcept {
  io_uring_operation_base* current_head =
      global_tasks_.load(std::memory_order_acquire);
  do {
    operation.next = current_head;
  } while (!global_tasks_.compare_exchange_weak(current_head, &operation,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire));
  notify_waiters();
}

void io_uring_context::push_global_tasks(operation_queue& operations) noexcept {
  io_uring_operation_base* ordered_tasks = reverse_tasks(operations.pop_all());
  if (ordered_tasks == nullptr) {
    return;
  }

  while (ordered_tasks != nullptr) {
    io_uring_operation_base* operation = ordered_tasks;
    ordered_tasks = ordered_tasks->next;

    io_uring_operation_base* current_head =
        global_tasks_.load(std::memory_order_acquire);
    do {
      operation->next = current_head;
    } while (!global_tasks_.compare_exchange_weak(current_head, operation,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire));
  }
  notify_waiters();
}

bool io_uring_context::move_global_tasks(
    operation_queue& local_tasks) noexcept {
  io_uring_operation_base* incoming =
      global_tasks_.exchange(nullptr, std::memory_order_acq_rel);
  if (incoming == nullptr) {
    return false;
  }

  local_tasks.push(reverse_tasks(incoming));
  return true;
}

void io_uring_context::notify_waiters() noexcept {
  std::lock_guard lock(wait_mutex_);
  wait_cv_.notify_all();
}

int io_uring_context::submit_wake_task() noexcept {
  std::lock_guard lock(uring_mutex_);
  return submit_wake_task_locked();
}

int io_uring_context::submit_wake_task_locked() noexcept {
  if (!ring_.is_open()) {
    return -EINVAL;
  }
  if (wake_task_pending_) {
    return 0;
  }

  bupp::base::submission_queue_entry sqe = ring_.get_sqe();
  if (sqe.raw() == nullptr) {
    return -EAGAIN;
  }

  sqe.prep_nop();
  sqe.set_data(wake_user_data());

  const int submit_result = ring_.submit();
  if (submit_result <= 0) {
    return submit_result < 0 ? submit_result : -EAGAIN;
  }

  wake_task_pending_ = true;
  return submit_result;
}

void* io_uring_context::wake_user_data() noexcept {
  static int wake_sentinel = 0;
  return &wake_sentinel;
}

int io_uring_context::wait_for_cqe_event() noexcept {
  int ring_fd = -1;
  {
    std::lock_guard lock(uring_mutex_);
    if (!ring_.is_open()) {
      return -EINVAL;
    }
    ring_fd = ring_.native_fd();
  }

  for (;;) {
    const int result = bupp::base::ring::wait_cqe_event(ring_fd, 1);
    if (result == -EINTR) {
      continue;
    }
    return result;
  }
}

bool io_uring_context::collect_ready_cqes(
    operation_queue& local_tasks) noexcept {
  operation_queue cqe_tasks;
  const unsigned task_count = collect_cqe_tasks(cqe_tasks);
  if (task_count == 0) {
    return false;
  }

  dispatch_cqe_tasks(cqe_tasks, task_count, local_tasks);
  return true;
}

unsigned io_uring_context::collect_cqe_tasks(
    operation_queue& cqe_tasks) noexcept {
  std::lock_guard lock(uring_mutex_);

  if (!ring_.is_open()) {
    return 0;
  }

  unsigned task_count = 0;
  (void)ring_.consume_ready_cqes(
      cqe_batch_window_, [this, &cqe_tasks, &task_count](
                             bupp::base::completion_queue_entry cqe) noexcept {
        cqe_data data;
        data.user_data = cqe.get_data();
        data.result = cqe.res();
        data.flags = cqe.flags();

        if (data.user_data == wake_user_data()) {
          wake_task_pending_ = false;
        }

        if (enqueue_cqe_task(data, cqe_tasks)) {
          ++task_count;
        }
      });
  return task_count;
}

void io_uring_context::dispatch_cqe_tasks(
    operation_queue& cqe_tasks, unsigned task_count,
    operation_queue& local_tasks) noexcept {
  if (task_count <= cqe_inline_completion_threshold_) {
    local_tasks.push(reverse_tasks(cqe_tasks.pop_all()));
    return;
  }

  push_global_tasks(cqe_tasks);
}

bool io_uring_context::enqueue_cqe_task(const cqe_data& data,
                                        operation_queue& tasks) noexcept {
  if (data.user_data == wake_user_data()) {
    return false;
  }

  auto* operation = static_cast<io_uring_operation_base*>(data.user_data);
  if (operation == nullptr) {
    return false;
  }

  operation->result = data.result;
  operation->flags = data.flags;
  tasks.push(*operation);
  return true;
}

bool io_uring_context::should_finish() const noexcept {
  return state_.load(std::memory_order_acquire) != context_state::running;
}

void io_uring_context::finish(operation_queue& local_tasks) noexcept {
  for (;;) {
    (void)move_global_tasks(local_tasks);
    (void)collect_ready_cqes(local_tasks);
    (void)move_global_tasks(local_tasks);

    io_uring_operation_base* operations = reverse_tasks(local_tasks.pop_all());
    if (operations == nullptr) {
      break;
    }
    execute_tasks(operations);
  }

  state_.store(context_state::finished, std::memory_order_release);
  notify_waiters();
}

}  // namespace bupp::async_io::linux_native
