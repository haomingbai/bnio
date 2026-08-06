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
    local_state_.push_cpu(reverse_tasks(event_tasks.pop_all()));
    return;
  }

  if (options_.local_queue_threshold == 0 ||
      (scheduling_state_.local_task_budget > 0 &&
       task_count <= scheduling_state_.local_task_budget)) {
    local_state_.push_cpu(reverse_tasks(event_tasks.pop_all()));
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
  // Re-link the fired node into its (ident, filter) wait queue. If the queue
  // is empty the node is armed again as the head; otherwise it queues behind
  // the current armed head and is armed when its turn arrives.
  const int append_result = append_node(node);
  if (append_result >= 0) {
    return true;
  }
  operation.result = append_result;
  return false;
}

bool kqueue_context::dispatch_event_result(
    kqueue_io_operation_base& operation, kqueue_registration_state& node,
    const bnio::base::event& event) noexcept {
  const kqueue_task task = node.task;
  const unsigned poll_mask = node.poll_mask;

  // Event-type dispatch:
  //   poll: convert the kevent filter/flags into a POLL mask.
  //   error (non-zero data): propagate the errno from the kevent.
  //   write EOF: translate to EPIPE or fflags.
  //   otherwise: perform the actual I/O step; retry on EAGAIN.
  //
  // Performs the native I/O step and returns whether the operation completed.
  // A false return means the operation was re-armed after EAGAIN and must
  // stay inflight.
  const auto perform_io_step = [&]() noexcept -> bool {
    operation.result =
        operation.owns_io_step() ? operation.perform_io() : -EOPNOTSUPP;
    if (operation.result == -EAGAIN || operation.result == -EWOULDBLOCK) {
      if (try_rearm_operation(operation, node)) {
        return false;  // re-armed; keep inflight
      }
    }
    return true;
  };

  switch (task) {
    case kqueue_task::poll:
      operation.result = static_cast<int>(poll_result(poll_mask, event));
      break;

    case kqueue_task::write:
      if (event.has_error() && event.data() != 0) {
        operation.result = -static_cast<int>(event.data());
      } else if (event.has_eof()) {
        operation.result =
            event.fflags() == 0 ? -EPIPE : -static_cast<int>(event.fflags());
      } else if (!perform_io_step()) {
        return false;
      }
      break;

    case kqueue_task::read:
    case kqueue_task::none:
    case kqueue_task::nop:
    default:
      // Error and EOF are handled per-task above; any other task value
      // performs the native I/O step, matching the original implicit-else
      // fallback.
      if (event.has_error() && event.data() != 0) {
        operation.result = -static_cast<int>(event.data());
      } else if (!perform_io_step()) {
        return false;
      }
      break;
  }
  return true;
}

bool kqueue_context::process_event(const bnio::base::event& event,
                                   operation_queue& tasks) noexcept {
  switch (classify_udata(event.udata())) {
    case event_udata_kind::shared_wake:
      // Drain the shared wake channel so edge-triggered EVFILT_READ can
      // re-fire on the next write. This handles both the legacy
      // EVFILT_USER path and the new shared-pipe path uniformly.
      if (global_state_ != nullptr) {
        (void)global_state_->wake_channel_.drain();
      }
      return false;

    case event_udata_kind::local_wake:
      // Directed wake: only this worker is signalled. Drain the per-worker
      // channel so the edge-triggered EVFILT_READ can fire again.
      (void)local_state_.wake_channel_.drain();
      return false;

    case event_udata_kind::operation:
      break;
  }

  auto* operation = static_cast<kqueue_io_operation_base*>(event.udata());
  if (operation == nullptr) {
    return false;
  }

  // udata is the operation pointer; locate the registration node for this
  // event's filter (an operation owns at most two nodes: READ + WRITE).
  kqueue_registration_state* node = nullptr;
  for (std::uint8_t index = 0; index < operation->registration_count; ++index) {
    kqueue_registration_state& candidate = operation->registrations[index];
    if (candidate.operation != nullptr && candidate.filter == event.filter()) {
      node = &candidate;
      break;
    }
  }
  if (node == nullptr) {
    return false;
  }

  // Explicitly EV_DELETE the level-triggered (EV_ADD) filter that delivered
  // this event so it does not re-fire before the next waiter is armed.
  {
    bnio::base::event deletion(node->ident, node->filter, EV_DELETE, 0, 0,
                               operation);
    (void)queue_.control(&deletion, 1, nullptr, 0, nullptr);
  }
  node->armed = false;

  // Unlink the fired head and arm the successor that takes its place.
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
  // Convert kevent filter/flags to a POLL mask. Only the bits that were
  // requested in poll_mask are set in the result.
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
