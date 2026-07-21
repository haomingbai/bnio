/**
 * @file kqueue_context_io_tasks.cpp
 * @brief IO task submission, registration slot management, arm/disarm, and
 * dedup.
 */

#include <bnio/async_io/bsd/kqueue_context.h>

#include <array>
#include <cerrno>
#include <climits>
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

    prepared_operation prepared;
    const int prepare_result = prepare_io(*operation, prepared);
    if (prepare_result < 0) {
      operation->result = prepare_result;
      operation->complete_submit_error(prepare_result);
      local_state_.push_cpu(*operation);
      continue;
    }

    bool complete_immediately = prepared.task == kqueue_task::nop;
    if (!complete_immediately) {
      const int register_result = register_operation(prepared);
      if (register_result < 0) {
        operation->result = register_result;
        operation->complete_submit_error(register_result);
        complete_immediately = true;
      }
    }

    if (complete_immediately) {
      local_state_.push_cpu(*operation);
    }
  }

  return true;
}

int kqueue_context::prepare_io(kqueue_io_operation_base& operation,
                               prepared_operation& prepared) noexcept {
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

  helper.set_udata(&operation);
  prepared.operation = &operation;
  prepared.event_count = helper.event_count();
  prepared.task = helper.task();
  prepared.poll_mask = helper.poll_mask();
  for (std::size_t index = 0; index < helper.event_count(); ++index) {
    prepared.events[index] = helper.event(index);
  }
  return 0;
}

active_registration* kqueue_context::find_free_registration_slot() noexcept {
  for (std::size_t index = 0; index < active_registration_capacity_; ++index) {
    if (active_registrations_[index].operation == nullptr) {
      return &active_registrations_[index];
    }
  }
  return nullptr;
}

bool kqueue_context::is_event_already_armed(
    std::uintptr_t ident, std::int16_t filter) const noexcept {
  for (std::size_t index = 0; index < active_registration_capacity_; ++index) {
    const active_registration& active = active_registrations_[index];
    if (active.operation != nullptr && active.armed &&
        active.event.ident() == ident && active.event.filter() == filter) {
      return true;
    }
  }
  return false;
}

int kqueue_context::register_operation(
    const prepared_operation& prepared) noexcept {
  if (prepared.operation == nullptr || prepared.event_count == 0 ||
      prepared.event_count > prepared.events.size()) {
    return -EBADF;
  }

  for (std::size_t index = 0; index < prepared.event_count; ++index) {
    if (prepared.events[index].ident() > static_cast<std::uintptr_t>(INT_MAX)) {
      return -EBADF;
    }
  }
  std::size_t free_count = 0;
  for (std::size_t index = 0; index < active_registration_capacity_; ++index) {
    if (active_registrations_[index].operation == nullptr) {
      ++free_count;
    }
  }
  if (free_count < prepared.event_count) {
    return -EAGAIN;
  }

  for (std::size_t index = 0; index < prepared.event_count; ++index) {
    active_registration* slot = find_free_registration_slot();
    if (slot == nullptr) {
      unregister_operation(*prepared.operation);
      return -EAGAIN;
    }
    // Allocate a slot for each event in the prepared operation.
    // Skip arm if an identical (ident, filter) registration is already
    // armed (dedup), otherwise add with EV_RECEIPT for synchronous
    // error detection.
    const bool already_armed = is_event_already_armed(
        prepared.events[index].ident(), prepared.events[index].filter());

    *slot = active_registration{prepared.operation,
                                prepared.events[index],
                                prepared.task,
                                prepared.poll_mask,
                                false,
                                next_registration_sequence_++};
    if (!already_armed) {
      const int arm_result = arm_registration(*slot);
      if (arm_result < 0) {
        unregister_operation(*prepared.operation);
        return arm_result;
      }
    }
  }
  return 0;
}

int kqueue_context::arm_registration(
    active_registration& registration) noexcept {
  // Use EV_RECEIPT to detect synchronous errors (e.g. invalid descriptor)
  // immediately, before the event loop would otherwise discover them.
  bnio::base::event change = registration.event;
  change.set_flags(change.flags() | EV_RECEIPT);
  change.set_udata(registration.operation);
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
  registration.armed = true;
  return 0;
}

void kqueue_context::arm_next_registration(std::uintptr_t descriptor,
                                           std::int16_t filter) noexcept {
  for (;;) {
    active_registration* next = nullptr;
    // Find the lowest-sequence unarmed registration on this
    // descriptor/filter pair so events are delivered in submission
    // order.
    for (std::size_t index = 0; index < active_registration_capacity_;
         ++index) {
      active_registration& active = active_registrations_[index];
      if (active.operation == nullptr || active.armed ||
          active.event.ident() != descriptor ||
          active.event.filter() != filter) {
        continue;
      }
      if (next == nullptr || active.sequence < next->sequence) {
        next = &active;
      }
    }
    if (next == nullptr) {
      return;
    }

    const int result = arm_registration(*next);
    if (result >= 0) {
      return;
    }

    kqueue_io_operation_base* operation = next->operation;
    *next = {};
    unregister_operation(*operation);
    operation->result = result;
    operation->complete_submit_error(result);
    local_state_.push_cpu(*operation);
  }
}

void kqueue_context::unregister_operation(
    kqueue_io_operation_base& operation) noexcept {
  std::array<std::pair<std::uintptr_t, std::int16_t>, 2> released{};
  std::size_t released_count = 0;
  // Clear every armed registration slot owned by this operation.
  // For each armed slot, issue EV_DELETE to remove the kevent filter,
  // then re-arm the next queued registration on the same ident/filter.
  for (std::size_t index = 0; index < active_registration_capacity_; ++index) {
    active_registration& active = active_registrations_[index];
    if (active.operation != &operation) {
      continue;
    }
    if (active.armed) {
      bnio::base::event deletion(active.event.ident(), active.event.filter(),
                                 EV_DELETE, 0, 0, &operation);
      (void)queue_.control(&deletion, 1, nullptr, 0, nullptr);
      released[released_count++] =
          std::pair(active.event.ident(), active.event.filter());
    }
    active = {};
  }
  for (std::size_t index = 0; index < released_count; ++index) {
    arm_next_registration(released[index].first, released[index].second);
  }
}

bool kqueue_context::take_registration(
    const bnio::base::event& event,
    active_registration& registration) noexcept {
  // Match the first armed registration by udata (operation pointer) plus
  // ident and filter. Clear the slot so it can be reused.
  for (std::size_t index = 0; index < active_registration_capacity_; ++index) {
    active_registration& active = active_registrations_[index];
    if (active.operation == event.udata() && active.armed &&
        active.event.ident() == event.ident() &&
        active.event.filter() == event.filter()) {
      registration = active;
      active = {};
      return true;
    }
  }
  return false;
}

}  // namespace bnio::async_io::bsd_native
