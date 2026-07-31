/**
 * @file kqueue_context_io_tasks.cpp
 * @brief IO task submission, registration, arm/disarm, and dedup.
 *
 * Registration state lives inside the I/O operations themselves
 * (kqueue_registration_state nodes). Wait queues are intrusive doubly-linked
 * lists keyed by (ident, filter); the queue tail is located by a linear scan
 * of the inflight list, and unlinking a node is O(1). No separate registration
 * table is allocated.
 */

#include <bnio/async_io/bsd/kqueue_context.h>

#include <cerrno>
#include <utility>

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
  kqueue_io_operation_base* queue_local =
      std::exchange(local_state_.io, nullptr);
  kqueue_io_operation_base* global_io =
      std::exchange(incoming_io_tasks_, nullptr);

  // Merge local and global IO queues. Both are MPSC-LIFO, so each is
  // reversed individually to restore FIFO producer order, then the global
  // list is appended to the local tail.
  kqueue_io_operation_base* operations = reverse_io_tasks(queue_local);
  kqueue_io_operation_base** tail = &operations;
  while (*tail != nullptr) {
    tail = &(*tail)->io_next;
  }
  *tail = reverse_io_tasks(global_io);
  while (*tail != nullptr) {
    tail = &(*tail)->io_next;
  }
  if (operations == nullptr) {
    return false;
  }

  while (operations != nullptr) {
    kqueue_io_operation_base* operation = operations;
    operations = operation->io_next;
    operation->io_next = nullptr;
    operation->result = 0;
    operation->flags = 0;

    const int prepare_result = prepare_io(*operation);
    if (prepare_result < 0) {
      operation->result = prepare_result;
      operation->complete_submit_error(prepare_result);
      local_state_.push_cpu(*operation);
      continue;
    }

    // nop tasks carry no registration nodes and complete immediately.
    bool complete_immediately = operation->registration_count == 0;
    if (!complete_immediately) {
      const int register_result = register_operation(*operation);
      if (register_result < 0) {
        operation->result = register_result;
        operation->complete_submit_error(register_result);
        complete_immediately = true;
      } else {
        add_inflight(*operation);
      }
    }

    if (complete_immediately) {
      local_state_.push_cpu(*operation);
    }
  }

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
  // nop completes without a native registration.
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
    node.sequence = next_registration_sequence_++;
  }
  operation.registration_count = static_cast<std::uint8_t>(event_count);
  return 0;
}

kqueue_registration_state* kqueue_context::find_queue_tail(
    std::uintptr_t ident, std::int16_t filter) const noexcept {
  // Linear scan of inflight operations. The tail of the (ident, filter) wait
  // queue is the registered node whose wait_next is null. The queue head is
  // always armed; a length-1 queue therefore also reports its (armed) head as
  // the tail.
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
    // Arming failed: capture the successor, then tear the operation down.
    // fail_operation detaches `candidate` (and the op's other nodes) without
    // re-arming, so the loop continues with the next candidate. A given op
    // contributes at most one node per (ident, filter) queue, so the
    // successor survives the teardown as the new queue head.
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
    // If the removed node was the armed head, arm the successor that took over.
    if (was_armed) {
      arm_queue_head(new_head);
    }
  }
  operation.registration_count = 0;
}

void kqueue_context::fail_operation(kqueue_io_operation_base& operation,
                                    int result) noexcept {
  // Detach all remaining nodes without re-arming (avoids unbounded recursion:
  // each failure completes exactly one operation).
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
  // Cancel every inflight operation: remove from list, EV_DELETE its
  // kqueue registrations, mark as cancelled, and push to the CPU queue.
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

  // Drain unregistered I/O from the global queue and the local incoming
  // buffer so they are completed instead of leaked.
  if (global_state_ != nullptr) {
    kqueue_io_operation_base* ops = global_state_->pop_io_all();
    while (ops != nullptr) {
      kqueue_io_operation_base* next = ops->io_next;
      ops->io_next = nullptr;
      ops->result = -ECANCELED;
      ops->complete_submit_stopped();
      local_state_.push_cpu(*ops);
      ops = next;
    }
  }

  kqueue_io_operation_base* unregistered =
      std::exchange(incoming_io_tasks_, nullptr);
  while (unregistered != nullptr) {
    kqueue_io_operation_base* next = unregistered->io_next;
    unregistered->io_next = nullptr;
    unregistered->result = -ECANCELED;
    unregistered->complete_submit_stopped();
    local_state_.push_cpu(*unregistered);
    unregistered = next;
  }
}

}  // namespace bnio::async_io::bsd_native
