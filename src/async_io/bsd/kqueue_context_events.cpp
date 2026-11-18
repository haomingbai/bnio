/**
 * @file kqueue_context_events.cpp
 * @brief Event collection, processing, dispatch, and re-arming logic.
 */

#include <bnio/async_io/bsd/kqueue_context.h>

#include <cerrno>
#include <cstddef>

#include "kqueue_context_internal.h"

namespace bnio::async_io::bsd_native {

bool kqueue_context::collect_ready_events(
    bool wait, const timespec* wait_timeout) noexcept {
  operation_queue event_tasks;
  const unsigned task_count =
      collect_event_tasks(event_tasks, wait, wait_timeout);
  if (task_count == 0) {
    return false;
  }

  dispatch_event_tasks(event_tasks, task_count);
  return true;
}

unsigned kqueue_context::collect_event_tasks(
    operation_queue& event_tasks, bool wait,
    const timespec* wait_timeout) noexcept {
  if (!queue_.is_open() || !event_buffer_) {
    return 0;
  }

  timespec no_wait{};
  const timespec* timeout = wait ? wait_timeout : &no_wait;
  int ready_count = 0;
  for (;;) {
    ready_count =
        queue_.control(nullptr, 0, event_buffer_.get(),
                       static_cast<int>(options_.event_batch_window), timeout);
    if (ready_count != -EINTR || !wait || should_finish()) {
      break;
    }
  }
  if (ready_count <= 0) {
    return 0;
  }

  unsigned task_count = 0;
  for (int index = 0; index < ready_count; ++index) {
    if (process_event(event_buffer_[static_cast<std::size_t>(index)],
                      event_tasks)) {
      ++task_count;
    }
  }
  return task_count;
}

void kqueue_context::dispatch_event_tasks(operation_queue& event_tasks,
                                          unsigned task_count) noexcept {
  if (task_count <= options_.event_inline_completion_threshold) {
    local_state_.push_cpu(event_tasks.pop_all());
    return;
  }

  if (options_.local_queue_threshold == 0 ||
      (scheduling_state_.local_task_budget > 0 &&
       task_count <= scheduling_state_.local_task_budget)) {
    local_state_.push_cpu(event_tasks.pop_all());
    if (options_.local_queue_threshold > 0) {
      scheduling_state_.local_task_budget -= task_count;
    }
    return;
  }

  push_cpu_tasks(event_tasks);
}

bool kqueue_context::try_rearm_operation(
    kqueue_io_operation_base& operation,
    kqueue_registration_state& node) noexcept {
  // Re-link the fired node into its (ident, filter) wait queue; append_node
  // arms it only when it becomes the head.
  const int append_result = append_node(node);
  if (append_result >= 0) {
    return true;
  }
  operation.result = append_result;
  return false;
}

bool kqueue_context::perform_io_step(kqueue_io_operation_base& operation,
                                     kqueue_registration_state& node) noexcept {
  // Native I/O step; false means re-armed after EAGAIN and still inflight.
  operation.result =
      operation.owns_io_step() ? operation.perform_io() : -EOPNOTSUPP;
  if (operation.result == -EAGAIN || operation.result == -EWOULDBLOCK) {
    if (try_rearm_operation(operation, node)) {
      return false;  // re-armed; keep inflight
    }
  }
  return true;
}

bool kqueue_context::dispatch_write_result(
    kqueue_io_operation_base& operation, kqueue_registration_state& node,
    const bnio::base::event& event) noexcept {
  if (event.has_error() && event.data() != 0) {
    operation.result = -static_cast<int>(event.data());
  } else if (event.has_eof()) {
    operation.result =
        event.fflags() == 0 ? -EPIPE : -static_cast<int>(event.fflags());
  } else if (!perform_io_step(operation, node)) {
    return false;
  }
  return true;
}

bool kqueue_context::dispatch_read_result(
    kqueue_io_operation_base& operation, kqueue_registration_state& node,
    const bnio::base::event& event) noexcept {
  if (event.has_error() && event.data() != 0) {
    operation.result = -static_cast<int>(event.data());
  } else if (!perform_io_step(operation, node)) {
    return false;
  }
  return true;
}

bool kqueue_context::dispatch_event_result(
    kqueue_io_operation_base& operation, kqueue_registration_state& node,
    const bnio::base::event& event) noexcept {
  const kqueue_task task = node.task;
  const unsigned poll_mask = node.poll_mask;

  switch (task) {
    case kqueue_task::poll:
      operation.result = static_cast<int>(poll_result(poll_mask, event));
      break;

    case kqueue_task::write:
      return dispatch_write_result(operation, node, event);

    case kqueue_task::read:
    case kqueue_task::none:
    case kqueue_task::nop:
    default:
      return dispatch_read_result(operation, node, event);
  }
  return true;
}

bool kqueue_context::drain_wake_channel(void* udata) noexcept {
  switch (classify_udata(udata)) {
    case event_udata_kind::shared_wake:
      // Drain the shared channel so its edge-triggered EVFILT_READ can
      // re-fire on the next write.
      if (global_state_ != nullptr) {
        (void)global_state_->wake_channel_.drain();
      }
      return true;

    case event_udata_kind::local_wake:
      // Directed wake: drain this worker's channel so it can fire again.
      (void)local_state_.wake_channel_.drain();
      return true;

    case event_udata_kind::operation:
      break;
  }
  return false;
}

kqueue_registration_state* kqueue_context::find_fired_node(
    kqueue_io_operation_base& operation, std::int16_t filter) noexcept {
  // An operation owns at most two nodes: READ and WRITE.
  for (std::uint8_t index = 0; index < operation.registration_count; ++index) {
    kqueue_registration_state& candidate = operation.registrations[index];
    if (candidate.operation != nullptr && candidate.filter == filter) {
      return &candidate;
    }
  }
  return nullptr;
}

bool kqueue_context::process_event(const bnio::base::event& event,
                                   operation_queue& tasks) noexcept {
  if (drain_wake_channel(event.udata())) {
    return false;
  }

  auto* operation = static_cast<kqueue_io_operation_base*>(event.udata());
  if (operation == nullptr) {
    return false;
  }

  kqueue_registration_state* node = find_fired_node(*operation, event.filter());
  if (node == nullptr) {
    return false;
  }

  // EV_DELETE the level-triggered filter so it cannot re-fire before re-arm.
  {
    bnio::base::event deletion(node->ident, node->filter, EV_DELETE, 0, 0,
                               operation);
    (void)queue_.control(&deletion, 1, nullptr, 0, nullptr);
  }
  node->armed = false;

  arm_queue_head(unlink_node(*node));

  operation->flags = event.flags();

  if (!dispatch_event_result(*operation, *node, event)) {
    return false;  // re-armed after EAGAIN; operation stays inflight
  }

  unregister_operation(*operation);
  remove_inflight(*operation);
  tasks.push(*operation);
  return true;
}

kqueue_context::event_udata_kind kqueue_context::classify_udata(
    void* udata) noexcept {
  if (udata == wakeup_user_data()) {
    return event_udata_kind::shared_wake;
  }
  if (udata == local_wakeup_user_data()) {
    return event_udata_kind::local_wake;
  }
  return event_udata_kind::operation;
}

unsigned kqueue_context::poll_result(
    unsigned poll_mask, const bnio::base::event& event) const noexcept {
  // Set only the mask bits that were requested in poll_mask.
  unsigned result = 0;
  if (event.filter() == EVFILT_READ) {
    result |= poll_mask & static_cast<unsigned>(POLLIN | POLLPRI);
#if defined(POLLRDNORM)
    result |= poll_mask & static_cast<unsigned>(POLLRDNORM);
#endif
#if defined(POLLRDBAND)
    result |= poll_mask & static_cast<unsigned>(POLLRDBAND);
#endif
  } else if (event.filter() == EVFILT_WRITE) {
    result |= poll_mask & static_cast<unsigned>(POLLOUT);
#if defined(POLLWRNORM)
    result |= poll_mask & static_cast<unsigned>(POLLWRNORM);
#endif
#if defined(POLLWRBAND)
    result |= poll_mask & static_cast<unsigned>(POLLWRBAND);
#endif
  }
  if (event.has_eof()) {
    result |= static_cast<unsigned>(POLLHUP);
  }
  if (event.has_error()) {
    result |= static_cast<unsigned>(POLLERR);
  }
  return result;
}

}  // namespace bnio::async_io::bsd_native
