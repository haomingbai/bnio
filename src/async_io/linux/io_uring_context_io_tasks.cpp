/**
 * @file io_uring_context_io_tasks.cpp
 * @brief I/O task submission: batch prepare, SQ-full retry slot, and error
 * handling.
 */

#include <bnio/async_io/linux/io_uring_context.h>
#include <bnio/base/linux/submission_queue_entry.h>

#include <cassert>
#include <cerrno>

#include "io_uring_context_internal.h"

namespace bnio::async_io::linux_native {

namespace {

/**
 * @brief Whether a submit error is plausibly transient.
 *
 * True when @p error leaves the ring usable (EINTR/EAGAIN/EBUSY/ENOMEM);
 * other errors would busy-spin the run loop if retried.
 */
[[nodiscard]] bool is_transient_submit_error(int error) noexcept {
  switch (error) {
    case -EINTR:
    case -EAGAIN:
    case -EBUSY:
    case -ENOMEM:
      return true;
    default:
      return false;
  }
}

}  // namespace

bool io_uring_context::consume_io_tasks() noexcept {
  // Teardown guard: while stopping or closing, abort instead of preparing —
  // the ring is going away and the completion contract requires the stop
  // channel, not a submit error. Implements stop()'s queued-work rule.
  if (stop_requested() || closing_requested()) {
    bool found = false;
    if (pending_io_retry_ != nullptr) {
      found = true;
      drain_io_list_complete_stopped(pending_io_retry_);
      pending_io_retry_ = nullptr;
    }
    if (io_uring_io_operation_base* operations = local_state_.pop_io_all()) {
      found = true;
      drain_io_list_complete_stopped(operations);
    }
    if (io_uring_io_operation_base* operations = global_state_->pop_io_all()) {
      found = true;
      drain_io_list_complete_stopped(operations);
    }
    return found;
  }

  io_uring_io_operation_base* prepared = nullptr;
  io_uring_io_operation_base* remaining = nullptr;
  bool found_work = false;

  // Priority: retry slot, then local (ownership), then shared; once the SQ
  // fills, leave untaken sources for other workers.
  io_uring_io_operation_base* operations = pending_io_retry_;
  pending_io_retry_ = nullptr;
  if (operations != nullptr) {
    found_work = true;
    remaining = prepare_io_batch(operations, prepared);
  }
  if (remaining == nullptr) {
    operations = local_state_.pop_io_all();
    if (operations != nullptr) {
      found_work = true;
      remaining = prepare_io_batch(operations, prepared);
    }
  }
  if (remaining == nullptr) {
    operations = global_state_->pop_io_all();
    if (operations != nullptr) {
      found_work = true;
      remaining = prepare_io_batch(operations, prepared);
    }
  }
  if (!found_work) {
    return false;
  }

  // One submit covers every SQE prepared from the sources above.
  const int submit_result = submit_and_track_prepared(prepared);
  if (submit_result < 0) {
    fail_io_list(prepared, submit_result);
    if (!is_transient_submit_error(submit_result)) {
      // Fatal: retrying would busy-spin; fail the remainder likewise.
      fail_io_list(remaining, submit_result);
      return true;
    }
    // Transient: the remainder never reached an SQE, so it keeps its
    // retry-slot turn.
  }
  // SQ-full remainder (nullptr when fully consumed): retried first on
  // the next run-loop pass.
  pending_io_retry_ = remaining;
  return true;
}

io_uring_io_operation_base* io_uring_context::prepare_io_batch(
    io_uring_io_operation_base* operations,
    io_uring_io_operation_base*& prepared) noexcept {
  // Prepare into SQEs; on EAGAIN return the rest so the caller submits what
  // is prepared and stashes the remainder. Other prepare errors fail inline.
  while (operations != nullptr) {
    io_uring_io_operation_base* operation = operations;
    const int prepare_result = prepare_io(*operation);
    if (prepare_result == -EAGAIN) {
      // SQ full: return this operation and the rest for the retry slot
      // instead of retrying inline.
      return operations;
    }

    operations = static_cast<io_uring_io_operation_base*>(operation->next);
    operation->next = nullptr;
    if (prepare_result < 0) {
      operation->complete_submit_error(prepare_result);
      local_state_.push_cpu(*operation);
    } else {
      operation->next = prepared;
      prepared = operation;
    }
  }
  return nullptr;
}

void io_uring_context::track_prepared_batch(
    io_uring_io_operation_base*& prepared) noexcept {
  // Register before the next pointers are cleared: an operation holding a
  // prepared SQE must always be reachable by abort_inflight_io().
  auto* current = prepared;
  while (current != nullptr) {
    auto* next_op = static_cast<io_uring_io_operation_base*>(current->next);
    add_inflight(*current);
    current = next_op;
  }
  while (prepared != nullptr) {
    io_uring_io_operation_base* operation = prepared;
    prepared = static_cast<io_uring_io_operation_base*>(operation->next);
    operation->next = nullptr;
  }
}

int io_uring_context::submit_and_track_prepared(
    io_uring_io_operation_base* prepared) noexcept {
  if (prepared == nullptr) {
    return 0;
  }
  // Track before the submit call; a failed submit untracks via fail_io_list().
  track_prepared_batch(prepared);
  return submit_ring();
}

void io_uring_context::fail_io_list(io_uring_io_operation_base* head,
                                    int result) noexcept {
  while (head != nullptr) {
    io_uring_io_operation_base* operation = head;
    head = static_cast<io_uring_io_operation_base*>(operation->next);
    operation->next = nullptr;
    // Undo track_prepared_batch(); a no-op for never-registered operations.
    remove_inflight(*operation);
    operation->complete_submit_error(result);
    local_state_.push_cpu(*operation);
  }
}

int io_uring_context::prepare_io(
    io_uring_io_operation_base& operation) noexcept {
  if (!ring_.is_open()) {
    return -EINVAL;
  }

  bnio::base::submission_queue_entry sqe = ring_.get_sqe();
  if (sqe.raw() == nullptr) {
    return -EAGAIN;
  }

  operation.prepare(sqe);
  sqe.set_data(static_cast<io_uring_operation_base*>(&operation));
  return 0;
}

int io_uring_context::submit_ring() noexcept {
  if (!ring_.is_open()) {
    return -EINVAL;
  }
  return ring_.submit();
}

int io_uring_context::enable_ring() noexcept {
  if (!ring_.is_open()) {
    return -EINVAL;
  }
  if (!run_state_.ring_disabled) {
    return 0;
  }

  const int result = ring_.enable();
  if (result >= 0) {
    run_state_.ring_disabled = false;
  }
  return result;
}

void io_uring_context::assert_running() const noexcept {
#ifndef NDEBUG
  const context_state s = run_state_.state.load(std::memory_order_acquire);
  assert(s == context_state::running || s == context_state::finishing);
#endif
}

void io_uring_context::add_inflight(
    io_uring_io_operation_base& operation) noexcept {
  if (operation.io_prev != nullptr || inflight_io_head_ == &operation) {
    return;
  }
  operation.io_prev = nullptr;
  operation.io_next = inflight_io_head_;
  if (inflight_io_head_ != nullptr) {
    inflight_io_head_->io_prev = &operation;
  }
  inflight_io_head_ = &operation;
}

void io_uring_context::remove_inflight(
    io_uring_io_operation_base& operation) noexcept {
  if (operation.io_prev == nullptr && inflight_io_head_ != &operation) {
    return;
  }
  if (operation.io_prev != nullptr) {
    operation.io_prev->io_next = operation.io_next;
  } else {
    inflight_io_head_ = operation.io_next;
  }
  if (operation.io_next != nullptr) {
    operation.io_next->io_prev = operation.io_prev;
  }
  operation.io_next = nullptr;
  operation.io_prev = nullptr;
}

void io_uring_context::abort_inflight_io() noexcept {
  while (inflight_io_head_ != nullptr) {
    io_uring_io_operation_base* op = inflight_io_head_;
    inflight_io_head_ = op->io_next;
    if (inflight_io_head_ != nullptr) {
      inflight_io_head_->io_prev = nullptr;
    }
    op->io_next = nullptr;
    op->io_prev = nullptr;

    op->result = -ECANCELED;
    op->complete_submit_stopped();
    local_state_.push_cpu(*op);
  }

  // Drain unregistered I/O (retry slot, then local and shared queues) so
  // callbacks that published here are completed instead of leaked.
  drain_io_list_complete_stopped(pending_io_retry_);
  pending_io_retry_ = nullptr;
  drain_io_list_complete_stopped(local_state_.pop_io_all());
  drain_io_list_complete_stopped(global_state_->pop_io_all());
}

void io_uring_context::drain_io_list_complete_stopped(
    io_uring_io_operation_base* head) noexcept {
  while (head != nullptr) {
    io_uring_io_operation_base* next =
        static_cast<io_uring_io_operation_base*>(head->next);
    head->next = nullptr;
    head->result = -ECANCELED;
    head->complete_submit_stopped();
    local_state_.push_cpu(*head);
    head = next;
  }
}

}  // namespace bnio::async_io::linux_native
