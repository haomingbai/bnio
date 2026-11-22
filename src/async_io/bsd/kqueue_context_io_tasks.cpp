/**
 * @file kqueue_context_io_tasks.cpp
 * @brief IO task submission, registration, arm/disarm, and dedup.
 *
 * Registration state lives in the I/O operations themselves; wait queues are
 * intrusive lists keyed by (ident, filter). No separate table is allocated.
 */

#include <bnio/async_io/bsd/kqueue_context.h>

#include <cerrno>

#include "kqueue_context_internal.h"

namespace bnio::async_io::bsd_native {
namespace {

// MPSC publication is LIFO; restore producer order before registering events.
[[nodiscard]] kqueue_io_operation_base* reverse_io_tasks(
    kqueue_io_operation_base* tasks) noexcept {
  kqueue_io_operation_base* reversed = nullptr;
  while (tasks != nullptr) {
    kqueue_io_operation_base* next = tasks->io_next;
    tasks->io_next = reversed;
    reversed = tasks;
    tasks = next;
  }
  return reversed;
}

}  // namespace

bool kqueue_context::consume_io_tasks() noexcept {
  // A phase-1 request observed here must drive phase 2: a receiver that keeps
  // republishing ready I/O would otherwise never reach should_finish(), so
  // stopping would never be published and the loop would spin forever.
  complete_pending_stop();

  // While stopping/closing, abort instead of registering: an armed EV_ADD
  // could hang stop(). complete_submit_stopped() delivers the stop channel
  // (operation_canceled, or set_stopped if the token raced).
  if (stop_requested() || closing_requested()) {
    bool found = false;
    if (kqueue_io_operation_base* operations = local_state_.pop_io_all()) {
      found = true;
      drain_io_list_complete_stopped(operations);
    }
    if (global_state_ != nullptr) {
      if (kqueue_io_operation_base* operations = global_state_->pop_io_all()) {
        found = true;
        drain_io_list_complete_stopped(operations);
      }
    }
    return found;
  }

  // Local queue first (keeps the operation on the worker that owns the
  // connection), then the shared queue; stop at the first non-empty source.
  kqueue_io_operation_base* operations = local_state_.pop_io_all();
  if (operations == nullptr && global_state_ != nullptr) {
    operations = global_state_->pop_io_all();
  }
  if (operations == nullptr) {
    return false;
  }

  // Publication is LIFO on both queues; restore producer order before
  // registering.
  operations = reverse_io_tasks(operations);

  while (operations != nullptr) {
    kqueue_io_operation_base* operation = operations;
    operations = operation->io_next;
    operation->io_next = nullptr;
    operation->result = 0;
    operation->flags = 0;

    if (!prepare_and_register_operation(*operation)) {
      local_state_.push_cpu(*operation);
    }
  }

  return true;
}

bool kqueue_context::prepare_and_register_operation(
    kqueue_io_operation_base& operation) noexcept {
  const int prepare_result = prepare_io(operation);
  if (prepare_result < 0) {
    operation.result = prepare_result;
    operation.complete_submit_error(prepare_result);
    return false;
  }

  // nop tasks carry no registration nodes and complete immediately.
  if (operation.registration_count == 0) {
    return false;
  }

  const int register_result = register_operation(operation);
  if (register_result < 0) {
    operation.result = register_result;
    operation.complete_submit_error(register_result);
    return false;
  }

  add_inflight(operation);
  return true;
}

int kqueue_context::prepare_io(kqueue_io_operation_base& operation) noexcept {
  if (!queue_.is_open()) {
    return -EINVAL;
  }

  kqueue_helper helper;
  operation.prepare(helper);
  if (helper.error() < 0) {
    return helper.error();
  }
  if (helper.task() == kqueue_task::none) {
    return -EINVAL;
  }

  operation.registration_count = 0;
  if (helper.task() == kqueue_task::nop) {
    return 0;
  }

  const std::uintptr_t ident = static_cast<std::uintptr_t>(helper.descriptor());
  const std::size_t event_count = helper.event_count();
  for (std::size_t index = 0; index < event_count; ++index) {
    kqueue_registration_state& node = operation.registrations[index];
    node = kqueue_registration_state{};
    node.operation = &operation;
    node.ident = ident;
    node.filter = helper.event(index).filter();
    node.task = helper.task();
    node.poll_mask = helper.poll_mask();
    node.sequence = scheduling_state_.next_registration_sequence++;
  }
  operation.registration_count = static_cast<std::uint8_t>(event_count);
  return 0;
}

kqueue_registration_state* kqueue_context::find_queue_tail(
    std::uintptr_t ident, std::int16_t filter) const noexcept {
  // Linear scan of inflight operations. The queue tail is the registered node
  // whose wait_next is null; the head is always armed, so a length-1 queue
  // reports its head as the tail.
  for (kqueue_io_operation_base* op = inflight_io_head_; op != nullptr;
       op = op->io_next) {
    for (std::uint8_t index = 0; index < op->registration_count; ++index) {
      kqueue_registration_state& node = op->registrations[index];
      if (node.operation != nullptr && node.ident == ident &&
          node.filter == filter && node.wait_next == nullptr) {
        return &node;
      }
    }
  }
  return nullptr;
}

int kqueue_context::append_node(kqueue_registration_state& node) noexcept {
  kqueue_registration_state* tail = find_queue_tail(node.ident, node.filter);
  if (tail == nullptr) {
    // Empty queue: this node becomes the armed head.
    node.wait_prev = nullptr;
    node.wait_next = nullptr;
    const int arm_result = arm_registration(node);
    if (arm_result < 0) {
      return arm_result;
    }
    return 0;
  }
  // A waiter is already armed for this (ident, filter); queue behind it.
  tail->wait_next = &node;
  node.wait_prev = tail;
  node.wait_next = nullptr;
  node.armed = false;
  return 0;
}

kqueue_registration_state* kqueue_context::unlink_node(
    kqueue_registration_state& node) noexcept {
  kqueue_registration_state* next = node.wait_next;
  if (node.wait_prev != nullptr) {
    node.wait_prev->wait_next = next;
  }
  if (next != nullptr) {
    next->wait_prev = node.wait_prev;
  }
  node.wait_prev = nullptr;
  node.wait_next = nullptr;
  return next;
}

int kqueue_context::arm_registration(kqueue_registration_state& node) noexcept {
  // EV_ADD (level-triggered) so already-ready data fires immediately;
  // EV_RECEIPT surfaces synchronous errors (e.g. invalid descriptor) at submit
  // time.
  bnio::base::event change(node.ident, node.filter, EV_ADD | EV_RECEIPT, 0, 0,
                           node.operation);
  bnio::base::event receipt;
  timespec no_wait{};
  const int result = queue_.control(&change, 1, &receipt, 1, &no_wait);
  if (result < 0) {
    return result;
  }
  if (result != 1) {
    return -EIO;
  }
  if (receipt.has_error() && receipt.data() != 0) {
    return -static_cast<int>(receipt.data());
  }
  node.armed = true;
  return 0;
}

int kqueue_context::register_operation(
    kqueue_io_operation_base& operation) noexcept {
  for (std::uint8_t index = 0; index < operation.registration_count; ++index) {
    kqueue_registration_state& node = operation.registrations[index];
    const int append_result = append_node(node);
    if (append_result < 0) {
      // Roll back every node of this operation registered so far.
      unregister_operation(operation);
      return append_result;
    }
  }
  return 0;
}

void kqueue_context::arm_queue_head(
    kqueue_registration_state* candidate) noexcept {
  while (candidate != nullptr) {
    const int result = arm_registration(*candidate);
    if (result >= 0) {
      return;
    }
    // Arming failed: capture the successor, then tear the operation down
    // without re-arming. An operation contributes at most one node per
    // (ident, filter) queue, so the successor survives as the new queue head.
    kqueue_registration_state* after = candidate->wait_next;
    fail_operation(*candidate->operation, result);
    candidate = after;
  }
}

void kqueue_context::unregister_operation(
    kqueue_io_operation_base& operation) noexcept {
  for (std::uint8_t index = 0; index < operation.registration_count; ++index) {
    kqueue_registration_state& node = operation.registrations[index];
    if (node.operation == nullptr) {
      continue;  // already reset
    }
    const bool was_armed = node.armed;
    if (was_armed) {
      bnio::base::event deletion(node.ident, node.filter, EV_DELETE, 0, 0,
                                 &operation);
      (void)queue_.control(&deletion, 1, nullptr, 0, nullptr);
    }
    kqueue_registration_state* new_head = unlink_node(node);
    node = kqueue_registration_state{};
    // If the removed node was the armed head, arm its successor.
    if (was_armed) {
      arm_queue_head(new_head);
    }
  }
  operation.registration_count = 0;
}

void kqueue_context::fail_operation(kqueue_io_operation_base& operation,
                                    int result) noexcept {
  // Detach all nodes without re-arming: each failure completes exactly one
  // operation, so this cannot recurse.
  for (std::uint8_t index = 0; index < operation.registration_count; ++index) {
    kqueue_registration_state& node = operation.registrations[index];
    if (node.operation == nullptr) {
      continue;
    }
    if (node.armed) {
      bnio::base::event deletion(node.ident, node.filter, EV_DELETE, 0, 0,
                                 &operation);
      (void)queue_.control(&deletion, 1, nullptr, 0, nullptr);
    }
    (void)unlink_node(node);
    node = kqueue_registration_state{};
  }
  operation.registration_count = 0;
  remove_inflight(operation);
  operation.result = result;
  operation.complete_submit_error(result);
  local_state_.push_cpu(operation);
}

void kqueue_context::add_inflight(
    kqueue_io_operation_base& operation) noexcept {
  // Guard: already in the inflight list.
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

void kqueue_context::remove_inflight(
    kqueue_io_operation_base& operation) noexcept {
  // Guard: not in the inflight list.
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

void kqueue_context::abort_inflight_io() noexcept {
  // Abort every inflight operation and push it to the CPU queue as canceled.
  while (inflight_io_head_ != nullptr) {
    kqueue_io_operation_base* op = inflight_io_head_;
    inflight_io_head_ = op->io_next;
    if (inflight_io_head_ != nullptr) {
      inflight_io_head_->io_prev = nullptr;
    }
    op->io_next = nullptr;
    op->io_prev = nullptr;

    for (std::uint8_t index = 0; index < op->registration_count; ++index) {
      kqueue_registration_state& node = op->registrations[index];
      if (node.operation == nullptr) {
        continue;
      }
      if (node.armed) {
        bnio::base::event deletion(node.ident, node.filter, EV_DELETE, 0, 0,
                                   op);
        (void)queue_.control(&deletion, 1, nullptr, 0, nullptr);
      }
      node = kqueue_registration_state{};
    }
    op->registration_count = 0;

    op->result = -ECANCELED;
    op->complete_submit_stopped();
    local_state_.push_cpu(*op);
  }

  // Drain still-unregistered I/O from both queues so callbacks that published
  // here are completed instead of leaked.
  drain_io_list_complete_stopped(local_state_.pop_io_all());
  if (global_state_ != nullptr) {
    drain_io_list_complete_stopped(global_state_->pop_io_all());
  }
}

void kqueue_context::drain_io_list_complete_stopped(
    kqueue_io_operation_base* head) noexcept {
  while (head != nullptr) {
    kqueue_io_operation_base* next = head->io_next;
    head->io_next = nullptr;
    head->result = -ECANCELED;
    head->complete_submit_stopped();
    local_state_.push_cpu(*head);
    head = next;
  }
}

}  // namespace bnio::async_io::bsd_native
