/**
 * @file io_uring_context_task_queue.cpp
 * @brief Task queue operations: post, publish, CPU push, wait/notify, and
 * wake channel I/O.
 */

#include <poll.h>

#include <cassert>
#include <cerrno>
#include <mutex>

#include "io_uring_context_internal.h"

namespace bnio::async_io::linux_native {

int io_uring_context::post(io_uring_operation_base& operation) noexcept {
  assert_running();

  if (current_context_ == this) {
    local_state_.push_cpu(operation);
    return 0;
  }

  global_state_->push_cpu(operation);
  notify_one_waiter();
  return 0;
}

void io_uring_context::publish_io(
    io_uring_io_operation_base& operation) noexcept {
  assert_running();
  global_state_->push_io(operation);
  notify_one_waiter();
}

void io_uring_context::push_cpu_tasks(operation_queue& operations) noexcept {
  io_uring_operation_base* tasks = operations.pop_all();
  if (tasks == nullptr) {
    return;
  }

  while (tasks != nullptr) {
    io_uring_operation_base* operation = tasks;
    tasks = tasks->next;
    operation->next = nullptr;
    global_state_->push_cpu(*operation);
  }
  notify_one_waiter();
}

io_uring_operation_base* io_uring_context::fetch_cpu_task() noexcept {
  // 1. Worker-local queue first: fastest and preserves locality.
  if (io_uring_operation_base* operations = local_state_.pop_cpu_all()) {
    return reverse_tasks(operations);
  }

  // 2. Shared CPU queue.
  if (io_uring_operation_base* operations = global_state_->pop_cpu_all()) {
    return reverse_tasks(operations);
  }

  return nullptr;
}

bool io_uring_context::run_cpu_batch() noexcept {
  if (io_uring_operation_base* operations = fetch_cpu_task()) {
    execute_tasks(operations);
    return true;
  }
  return false;
}

void io_uring_context::unregister_local_state() noexcept {
  // Unlink from the suspend list if the worker is still linked there (a
  // run() that exits through the normal wait path already unlinked in
  // end_wait()). Only the suspend list exists now: without stealing no
  // remote thread ever touches a worker's local state while it runs.
  io_uring_worker_state_list& suspend = global_state_->workers;
  std::lock_guard<std::mutex> guard(suspend.lock);
  if (io_uring_local_state_in_list(suspend, &local_state_)) {
    io_uring_unlink_local_state(suspend, &local_state_);
  }
}

void io_uring_context::notify_one_waiter() noexcept {
  if (is_waiting()) {
    (void)signal_eventfd();
  }
}

bool io_uring_context::is_waiting() const noexcept {
  return run_state_.waiting.load(std::memory_order_acquire);
}

void io_uring_context::begin_wait() noexcept {
  run_state_.waiting.store(true, std::memory_order_release);
  // Link into the suspend list so a publisher can find and wake this
  // worker. No run list exists anymore (stealing was removed), so this is
  // a single locked insertion.
  {
    io_uring_worker_state_list& suspend = global_state_->workers;
    std::lock_guard<std::mutex> guard(suspend.lock);
    io_uring_link_local_state(suspend, &local_state_);
  }
  [[maybe_unused]] const std::size_t previous =
      global_state_->awake_workers.fetch_sub(1, std::memory_order_acq_rel);
  assert(previous != 0);
}

void io_uring_context::end_wait() noexcept {
  // Unlink from the suspend list; the worker is awake again.
  {
    io_uring_worker_state_list& suspend = global_state_->workers;
    std::lock_guard<std::mutex> guard(suspend.lock);
    io_uring_unlink_local_state(suspend, &local_state_);
  }
  global_state_->awake_workers.fetch_add(1, std::memory_order_acq_rel);
  run_state_.waiting.store(false, std::memory_order_release);
}

int io_uring_context::signal_eventfd() noexcept {
  // Bind the shared wake-channel write to the submit lock so the
  // io_context destructor's close (under the same lock) can never race
  // it. Re-check after acquiring the lock: the channel may have been
  // closed while we waited for the lock.
  std::lock_guard guard(global_state_->submit_lock);
  if (!global_state_->wake_channel_.is_open()) {
    return -EBADF;
  }
  return global_state_->wake_channel_.wake();
}

void io_uring_context::drain_eventfd() noexcept {
  (void)global_state_->wake_channel_.drain();
}

int io_uring_context::arm_wake_poll(int fd, void* user_data,
                                    bool& pending_flag) noexcept {
  // read_fd() is -1 once the channel is closed, so this also covers a
  // channel that was torn down underneath the run loop.
  if (!ring_.is_open() || fd < 0) {
    return -EINVAL;
  }
  if (pending_flag) {
    return 1;
  }
  if (run_state_.state.load(std::memory_order_acquire) !=
      context_state::running) {
    // Distinguishable from "already armed": the poll is NOT armed here
    // because the context is stopping. Callers must not block on the
    // ring without another wake source (inflight completions or a
    // bounded timeout) after seeing this return value.
    return 0;
  }

  bnio::base::submission_queue_entry sqe = ring_.get_sqe();
  if (sqe.raw() == nullptr) {
    // SQ is full. There is no inline retry: the caller's -EAGAIN path
    // re-enters the run loop, which re-arms on the next pass.
    return -EAGAIN;
  }

  sqe.prep_poll_add(fd, static_cast<unsigned>(POLLIN));
  sqe.set_data(user_data);

  const int submit_result = submit_ring();
  if (submit_result <= 0) {
    return submit_result < 0 ? submit_result : -EAGAIN;
  }

  pending_flag = true;
  return 1;
}

void* io_uring_context::eventfd_user_data() noexcept {
  static int eventfd_sentinel = 0;
  return &eventfd_sentinel;
}

void* io_uring_context::local_eventfd_user_data() noexcept {
  static int local_eventfd_sentinel = 0;
  return &local_eventfd_sentinel;
}

}  // namespace bnio::async_io::linux_native
