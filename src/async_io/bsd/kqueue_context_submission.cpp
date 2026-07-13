#include <bupp/async_io/bsd/kqueue_context.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <climits>

#include "kqueue_context_internal.h"

namespace bupp::async_io::bsd_native {
namespace {

[[nodiscard]] int set_nonblocking(int descriptor) noexcept {
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0) {
    return -errno;
  }
  if ((flags & O_NONBLOCK) != 0) {
    return 0;
  }
  return ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0 ? -errno : 0;
}

}  // namespace

int kqueue_context::submit() noexcept {
  assert_running();
  std::lock_guard lock(submission_mutex_);
  return submit_locked();
}

int kqueue_context::submit_locked() noexcept {
  if (!queue_.is_open()) {
    return -EINVAL;
  }

  const std::size_t submission_count = prepared_count_;
  operation_queue completed;
  for (std::size_t index = 0; index < submission_count; ++index) {
    prepared_operation& prepared = prepared_operations_[index];
    kqueue_operation_base& operation = *prepared.operation;
    operation.result = 0;
    operation.flags = 0;

    bool complete_immediately = prepared.task == kqueue_task::nop;
    if (prepared.task == kqueue_task::read ||
        prepared.task == kqueue_task::write) {
      const buffer_view data = operation.get_data();
      if (data.size > 0 && data.data == nullptr) {
        operation.result = -EFAULT;
        complete_immediately = true;
      } else if (data.size == 0) {
        complete_immediately = true;
      }
    }

    if (!complete_immediately) {
      const int register_result = register_operation(prepared);
      if (register_result < 0) {
        operation.result = register_result;
        complete_immediately = true;
      }
    }

    if (complete_immediately) {
      completed.push(operation);
    }
    prepared = {};
  }
  prepared_count_ = 0;

  if (!completed.empty()) {
    push_posted_tasks(completed);
  }
  return static_cast<int>(submission_count);
}

int kqueue_context::register_operation(
    const prepared_operation& prepared) noexcept {
  if (prepared.operation == nullptr || prepared.event_count == 0 ||
      prepared.event_count > prepared.events.size()) {
    return -EBADF;
  }

  const std::uintptr_t descriptor = prepared.events[0].ident();
  if (descriptor > static_cast<std::uintptr_t>(INT_MAX)) {
    return -EBADF;
  }
  if (prepared.task == kqueue_task::read ||
      prepared.task == kqueue_task::write) {
    const int nonblocking_result =
        set_nonblocking(static_cast<int>(descriptor));
    if (nonblocking_result < 0) {
      return nonblocking_result;
    }
  }

  std::array<bupp::base::event, 2> changes{};
  std::array<bupp::base::event, 2> receipts{};
  const int change_count = static_cast<int>(prepared.event_count);
  for (std::size_t index = 0; index < prepared.event_count; ++index) {
    changes[index] = prepared.events[index];
    changes[index].set_flags(changes[index].flags() | EV_RECEIPT);
  }

  {
    std::lock_guard lock(registrations_mutex_);
    for (std::size_t index = 0; index < prepared.event_count; ++index) {
      for (std::size_t active_index = 0;
           active_index < active_registration_capacity_; ++active_index) {
        const active_registration& active = active_registrations_[active_index];
        if (active.operation != nullptr &&
            active.descriptor == changes[index].ident() &&
            active.filter == changes[index].filter()) {
          return -EBUSY;
        }
      }
    }

    for (std::size_t index = 0; index < prepared.event_count; ++index) {
      active_registration* slot = nullptr;
      for (std::size_t active_index = 0;
           active_index < active_registration_capacity_; ++active_index) {
        if (active_registrations_[active_index].operation == nullptr) {
          slot = &active_registrations_[active_index];
          break;
        }
      }
      if (slot == nullptr) {
        for (std::size_t active_index = 0;
             active_index < active_registration_capacity_; ++active_index) {
          active_registration& active = active_registrations_[active_index];
          if (active.operation == prepared.operation) {
            active = {};
          }
        }
        return -EAGAIN;
      }
      *slot = active_registration{prepared.operation, changes[index].ident(),
                                  changes[index].filter(), prepared.task,
                                  prepared.poll_mask};
    }
  }

  timespec no_wait{};
  const int receipt_count = queue_.control(
      changes.data(), change_count, receipts.data(), change_count, &no_wait);
  if (receipt_count < 0) {
    unregister_operation(*prepared.operation);
    return receipt_count;
  }
  if (receipt_count != change_count) {
    unregister_operation(*prepared.operation);
    return -EIO;
  }

  int first_error = 0;
  for (int index = 0; index < receipt_count; ++index) {
    const bupp::base::event& receipt =
        receipts[static_cast<std::size_t>(index)];
    if (receipt.has_error() && receipt.data() != 0) {
      if (first_error == 0) {
        first_error = -static_cast<int>(receipt.data());
      }
    }
  }

  if (first_error < 0) {
    unregister_operation(*prepared.operation);
    return first_error;
  }
  return 0;
}

void kqueue_context::unregister_operation(
    kqueue_operation_base& operation) noexcept {
  std::lock_guard lock(registrations_mutex_);
  for (std::size_t index = 0; index < active_registration_capacity_; ++index) {
    active_registration& active = active_registrations_[index];
    if (active.operation != &operation) {
      continue;
    }
    bupp::base::event deletion(active.descriptor, active.filter, EV_DELETE, 0,
                               0, &operation);
    (void)queue_.control(&deletion, 1, nullptr, 0, nullptr);
    active = {};
  }
}

bool kqueue_context::take_registration(
    const bupp::base::event& event,
    active_registration& registration) noexcept {
  std::lock_guard lock(registrations_mutex_);
  for (std::size_t index = 0; index < active_registration_capacity_; ++index) {
    active_registration& active = active_registrations_[index];
    if (active.operation == event.udata() &&
        active.descriptor == event.ident() && active.filter == event.filter()) {
      registration = active;
      active = {};
      return true;
    }
  }
  return false;
}

}  // namespace bupp::async_io::bsd_native
