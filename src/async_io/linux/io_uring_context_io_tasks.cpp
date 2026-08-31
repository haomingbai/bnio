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

bool io_uring_context::consume_io_tasks() noexcept {
  // A pending SQ-full retry comes first: the unsubmitted remainder of
  // the previous batch is retried before any queue is popped. Only with
  // no retry outstanding, take the worker-local queue first (fastest,
  // keeps the operation on the worker that owns the connection), then
  // the shared queue — stop at the first non-empty source, exactly like
  // fetch_cpu_task().
  io_uring_io_operation_base* operations = pending_io_retry_;
  pending_io_retry_ = nullptr;
  if (operations == nullptr) {
    operations = local_state_.pop_io_all();
    if (operations == nullptr) {
      operations = global_state_->pop_io_all();
    }
  }
  if (operations == nullptr) {
    return false;
  }

  io_uring_io_operation_base* prepared = nullptr;
  // Prepare SQEs in a batch. If the SQ is full (EAGAIN), submit the
  // prepared entries and stash the remaining operations in the retry
  // slot instead of retrying inline. On any submission error, fail all
  // prepared and remaining operations.
  const auto release_prepared = [&prepared]() noexcept {
    while (prepared != nullptr) {
      io_uring_io_operation_base* operation = prepared;
      prepared = static_cast<io_uring_io_operation_base*>(operation->next);
      operation->next = nullptr;
    }
  };
  const auto fail_prepared = [this, &prepared](int result) noexcept {
    while (prepared != nullptr) {
      io_uring_io_operation_base* operation = prepared;
      prepared = static_cast<io_uring_io_operation_base*>(operation->next);
      operation->next = nullptr;
      operation->complete_submit_error(result);
      local_state_.push_cpu(*operation);
    }
  };
  // Submit the prepared SQEs. On success, register every prepared
  // operation in the inflight list before release_prepared clears
  // their next pointers; on failure the caller decides how to fail
  // the operations.
  const auto submit_and_track_prepared = [this, &prepared,
                                          &release_prepared]() noexcept -> int {
    const int result = submit_ring();
    if (result >= 0) {
      // Add every prepared operation to the inflight list before
      // release_prepared clears their next pointers.
      auto* current = prepared;
      while (current != nullptr) {
        auto* next_op = static_cast<io_uring_io_operation_base*>(current->next);
        add_inflight(*current);
        current = next_op;
      }
      release_prepared();
    }
    return result;
  };
  const auto fail_remaining = [this](io_uring_io_operation_base* head,
                                     int result) noexcept {
    while (head != nullptr) {
      io_uring_io_operation_base* operation = head;
      head = static_cast<io_uring_io_operation_base*>(operation->next);
      operation->next = nullptr;
      operation->complete_submit_error(result);
      local_state_.push_cpu(*operation);
    }
  };

  // Prepare each I/O operation into an SQE. Either batch it into the
  // prepared list or, on EAGAIN, hand the prepared SQEs to the kernel and
  // stash the rest in the run-loop retry slot. A submit failure fails all
  // remaining operations inline.
  while (operations != nullptr) {
    io_uring_io_operation_base* operation = operations;
    const int prepare_result = prepare_io(*operation);
    if (prepare_result == -EAGAIN) {
      // SQ is full: hand the prepared SQEs to the kernel, then stash
      // this operation and everything behind it in the retry slot
      // instead of retrying inline. The next run-loop pass picks the
      // list up before touching any queue — no reversal, no re-push,
      // and no wakeup (the publisher is the consumer).
      const int submit_result = submit_and_track_prepared();
      if (submit_result < 0) {
        fail_prepared(submit_result);
        fail_remaining(operations, submit_result);
        return true;
      }
      pending_io_retry_ = operations;
      return true;
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

  if (prepared != nullptr) {
    const int submit_result = submit_and_track_prepared();
    if (submit_result < 0) {
      fail_prepared(submit_result);
    }
  }
  return true;
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

  // Drain the still-unregistered I/O — the SQ-full retry slot first
  // (it was logically next in line), then this worker's local I/O queue
  // and the shared one — so they are completed instead of leaked.
  // Callbacks running here may have published I/O that never reached a
  // consume_io_tasks() pass.
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
