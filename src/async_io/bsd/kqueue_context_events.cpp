#include <bupp/async_io/bsd/kqueue_context.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>

#include "kqueue_context_internal.h"

namespace bupp::async_io::bsd_native {

bool kqueue_context::collect_ready_events(operation_queue& local_tasks,
                                          unsigned& local_task_budget,
                                          bool wait) noexcept {
  operation_queue event_tasks;
  const unsigned task_count = collect_event_tasks(event_tasks, wait);
  if (task_count == 0) {
    return false;
  }

  dispatch_event_tasks(event_tasks, task_count, local_tasks, local_task_budget);
  return true;
}

unsigned kqueue_context::collect_event_tasks(operation_queue& event_tasks,
                                             bool wait) noexcept {
  if (!queue_.is_open() || !event_buffer_) {
    return 0;
  }

  timespec no_wait{};
  const timespec* timeout = wait ? nullptr : &no_wait;
  int ready_count = 0;
  for (;;) {
    ready_count =
        queue_.control(nullptr, 0, event_buffer_.get(),
                       static_cast<int>(event_batch_window_), timeout);
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

void kqueue_context::dispatch_event_tasks(
    operation_queue& event_tasks, unsigned task_count,
    operation_queue& local_tasks, unsigned& local_task_budget) noexcept {
  if (task_count <= event_inline_completion_threshold_) {
    local_tasks.push(reverse_tasks(event_tasks.pop_all()));
    return;
  }

  if (local_queue_threshold_ == 0 ||
      (local_task_budget > 0 && task_count <= local_task_budget)) {
    local_tasks.push(reverse_tasks(event_tasks.pop_all()));
    if (local_queue_threshold_ > 0) {
      local_task_budget -= task_count;
    }
    return;
  }

  push_posted_tasks(event_tasks);
}

bool kqueue_context::process_event(const bupp::base::event& event,
                                   operation_queue& tasks) noexcept {
  if (event.udata() == wakeup_user_data()) {
    return false;
  }

  active_registration registration;
  if (!take_registration(event, registration) ||
      registration.operation == nullptr) {
    return false;
  }

  kqueue_operation_base& operation = *registration.operation;
  operation.flags = event.flags();

  if (registration.task == kqueue_task::poll) {
    operation.result =
        static_cast<int>(poll_result(registration.poll_mask, event));
  } else if (event.has_error() && event.data() != 0) {
    operation.result = -static_cast<int>(event.data());
  } else if (registration.task == kqueue_task::write && event.has_eof()) {
    operation.result =
        event.fflags() == 0 ? -EPIPE : -static_cast<int>(event.fflags());
  } else {
    operation.result = perform_io(operation, registration.task,
                                  static_cast<int>(registration.descriptor));
    if (operation.result == -EAGAIN || operation.result == -EWOULDBLOCK) {
      prepared_operation rearm;
      rearm.operation = &operation;
      rearm.event_count = 1;
      rearm.task = registration.task;
      rearm.poll_mask = registration.poll_mask;
      rearm.events[0].set(registration.descriptor, registration.filter,
                          EV_ADD | EV_ONESHOT, 0, 0, &operation);
      const int rearm_result = register_operation(rearm);
      if (rearm_result >= 0) {
        return false;
      }
      operation.result = rearm_result;
    }
  }

  unregister_operation(operation);
  tasks.push(operation);
  return true;
}

int kqueue_context::perform_io(kqueue_operation_base& operation,
                               kqueue_task task, int descriptor) noexcept {
  const buffer_view data = operation.get_data();
  if (data.size > 0 && data.data == nullptr) {
    return -EFAULT;
  }

  const std::size_t size =
      std::min(data.size, static_cast<std::size_t>(INT_MAX));
  for (;;) {
    ssize_t result = -1;
    if (task == kqueue_task::read) {
      result = ::read(descriptor, data.data, size);
    } else if (task == kqueue_task::write) {
      result = ::write(descriptor, data.data, size);
    } else {
      return -EINVAL;
    }

    if (result >= 0) {
      return static_cast<int>(result);
    }
    if (errno == EINTR) {
      continue;
    }
    return -errno;
  }
}

unsigned kqueue_context::poll_result(
    unsigned poll_mask, const bupp::base::event& event) const noexcept {
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

}  // namespace bupp::async_io::bsd_native
