#pragma once
#ifndef BNIO_TESTS_ASYNC_IO_IO_URING_CONTEXT_TEST_SUPPORT_H_
#define BNIO_TESTS_ASYNC_IO_IO_URING_CONTEXT_TEST_SUPPORT_H_

#include <bnio/async_io/linux/io_uring_context.h>
#include <bnio/base/linux/liburing.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <barrier>
#include <bexec/operation_state.hpp>
#include <bexec/sender.hpp>
#include <bexec/stop_token.hpp>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

namespace bnio_async_io_io_uring_test {

using bnio::async_io::buffer_view;
using bnio::async_io::datagram_socket_view;
using bnio::async_io::descriptor_view;
using bnio::async_io::stream_socket_view;
using bnio::async_io::linux_native::buffer_sequence_view;
using bnio::async_io::linux_native::const_message_view;
using bnio::async_io::linux_native::io_uring_accept_operation;
using bnio::async_io::linux_native::io_uring_connect_operation;
using bnio::async_io::linux_native::io_uring_context;
using bnio::async_io::linux_native::io_uring_context_options;
using bnio::async_io::linux_native::io_uring_datagram_receive_operation;
using bnio::async_io::linux_native::io_uring_datagram_send_operation;
using bnio::async_io::linux_native::io_uring_nop_operation;
using bnio::async_io::linux_native::io_uring_operation_base;
using bnio::async_io::linux_native::io_uring_poll_operation;
using bnio::async_io::linux_native::io_uring_post_operation;
using bnio::async_io::linux_native::io_uring_read_operation;
using bnio::async_io::linux_native::io_uring_readv_operation;
using bnio::async_io::linux_native::io_uring_receive_from_operation;
using bnio::async_io::linux_native::io_uring_recv_operation;
using bnio::async_io::linux_native::io_uring_recvmsg_operation;
using bnio::async_io::linux_native::io_uring_resolve_operation;
using bnio::async_io::linux_native::io_uring_send_operation;
using bnio::async_io::linux_native::io_uring_send_to_operation;
using bnio::async_io::linux_native::io_uring_sendmsg_operation;
using bnio::async_io::linux_native::io_uring_task_queue_state;
using bnio::async_io::linux_native::io_uring_timeout_operation;
using bnio::async_io::linux_native::io_uring_write_operation;
using bnio::async_io::linux_native::io_uring_writev_operation;
using bnio::async_io::linux_native::mutable_message_view;

enum class signal_kind {
  none,
  value,
  error,
  stopped,
};

struct shared_state {
  signal_kind signal = signal_kind::none;
  int result = 0;
  unsigned flags = 0;
  bool in_context = false;
  std::error_code error;
};

struct receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  io_uring_context* context = nullptr;
  bool stop_on_completion = false;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      state->signal = signal_kind::error;
      state->error = ec;
    } else {
      state->signal = signal_kind::value;
    }
    state->in_context = (context != nullptr && context->is_in_context());
    if (stop_on_completion && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_value(std::error_code ec, int result, unsigned flags) noexcept {
    if (ec) {
      state->signal = signal_kind::error;
      state->error = ec;
    } else {
      state->signal = signal_kind::value;
      state->result = result;
      state->flags = flags;
    }
    state->in_context = (context != nullptr && context->is_in_context());
    if (stop_on_completion && context != nullptr) {
      (void)context->stop();
    }
  }

  // set_error 已合并到 set_value(ec, ...) 的 ec 分支

  void set_stopped() noexcept {
    // 仅 io_context::stop() 触发
    state->signal = signal_kind::stopped;
    state->in_context = (context != nullptr && context->is_in_context());
    if (stop_on_completion && context != nullptr) {
      (void)context->stop();
    }
  }
};

struct poll_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  io_uring_context* context = nullptr;

  void set_value(std::error_code ec, unsigned events) noexcept {
    if (ec) {
      state->signal = signal_kind::error;
      state->error = ec;
    } else {
      state->signal = signal_kind::value;
      state->result = static_cast<int>(events);
    }
    state->in_context = (context != nullptr && context->is_in_context());
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  // set_error 已合并到 set_value(ec, ...) 的 ec 分支

  void set_stopped() noexcept {
    // 仅 io_context::stop() 触发
    state->signal = signal_kind::stopped;
    state->in_context = (context != nullptr && context->is_in_context());
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct resolve_state {
  signal_kind signal = signal_kind::none;
  std::size_t endpoint_count = 0;
  bool in_context = false;
  std::error_code error;
};

struct resolve_receiver {
  resolve_state* state = nullptr;
  io_uring_context* context = nullptr;

  void set_value(std::error_code ec, std::size_t count) noexcept {
    if (state != nullptr) {
      if (ec) {
        state->signal = signal_kind::error;
        state->error = ec;
      } else {
        state->signal = signal_kind::value;
        state->endpoint_count = count;
      }
      state->in_context = (context != nullptr && context->is_in_context());
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  // set_error 已合并到 set_value(ec, ...) 的 ec 分支

  void set_stopped() noexcept {
    // 仅 io_context::stop() 触发
    if (state != nullptr) {
      state->signal = signal_kind::stopped;
      state->in_context = (context != nullptr && context->is_in_context());
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct stop_env {
  bexec::inplace_stop_token token;

  [[nodiscard]] bexec::inplace_stop_token query(
      bexec::get_stop_token_t) const noexcept {
    return token;
  }
};

struct stopped_receiver : receiver {
  stop_env env;

  [[nodiscard]] stop_env get_env() const noexcept { return env; }
};

struct batch_state {
  unsigned completed = 0;
  unsigned errors = 0;
  unsigned stopped = 0;
  bool all_in_context = true;
  bool in_order = true;
  unsigned next_index = 0;
};

struct batch_receiver {
  std::shared_ptr<batch_state> state = std::make_shared<batch_state>();
  io_uring_context* context = nullptr;
  unsigned target = 0;

  void set_value(std::error_code ec, int result, unsigned /*flags*/) noexcept {
    if (ec) {
      ++state->errors;
    } else {
      EXPECT_TRUE(result == 0);
      ++state->completed;
    }
    state->all_in_context = state->all_in_context &&
                            (context != nullptr && context->is_in_context());
    // 成功时等 completed==target 再 stop；错误时立即 stop（与原 set_error
    // 一致）
    if ((ec || state->completed == target) && context != nullptr) {
      (void)context->stop();
    }
  }

  // set_error 已合并到 set_value(ec, ...) 的 ec 分支

  void set_stopped() noexcept {
    ++state->stopped;
    state->all_in_context = state->all_in_context &&
                            (context != nullptr && context->is_in_context());
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct post_batch_receiver {
  std::shared_ptr<batch_state> state = std::make_shared<batch_state>();
  io_uring_context* context = nullptr;
  unsigned target = 0;
  unsigned index = 0;

  void set_value(std::error_code ec) noexcept {
    if (ec) {
      ++state->errors;
    } else {
      state->in_order = state->in_order && index == state->next_index;
      ++state->next_index;
      ++state->completed;
    }
    state->all_in_context = state->all_in_context &&
                            (context != nullptr && context->is_in_context());
    // 成功时等 completed==target 再 stop；错误时立即 stop（与原 set_error
    // 一致）
    if ((ec || state->completed == target) && context != nullptr) {
      (void)context->stop();
    }
  }

  // set_error 已合并到 set_value(ec, ...) 的 ec 分支

  void set_stopped() noexcept {
    ++state->stopped;
    state->all_in_context = state->all_in_context &&
                            (context != nullptr && context->is_in_context());
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct concurrent_batch_state {
  std::atomic<unsigned> completed{0};
  std::atomic<unsigned> errors{0};
  std::atomic<unsigned> stopped{0};
};

struct concurrent_batch_receiver {
  std::shared_ptr<concurrent_batch_state> state =
      std::make_shared<concurrent_batch_state>();
  io_uring_context* context = nullptr;
  unsigned target = 0;

  void set_value(std::error_code ec, int result, unsigned /*flags*/) noexcept {
    if (ec) {
      state->errors.fetch_add(1, std::memory_order_acq_rel);
      if (context != nullptr) {
        (void)context->stop();
      }
    } else {
      EXPECT_TRUE(result == 0);
      const unsigned completed =
          state->completed.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (completed == target && context != nullptr) {
        (void)context->stop();
      }
    }
  }

  // set_error 已合并到 set_value(ec, ...) 的 ec 分支

  void set_stopped() noexcept {
    state->stopped.fetch_add(1, std::memory_order_acq_rel);
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

inline bool is_unsupported_ring_error(int result) {
  return result == -ENOSYS || result == -EPERM || result == -EACCES;
}

// Not thread-safe: callers only use this before publishing the state.
inline void reset_task_queue_state(
    io_uring_task_queue_state& global_tasks) noexcept {
  global_tasks.life_state.store(0, std::memory_order_relaxed);
  global_tasks.cpu_head.store(nullptr, std::memory_order_relaxed);
  global_tasks.io_head.store(nullptr, std::memory_order_relaxed);
  global_tasks.awake_workers.store(0, std::memory_order_relaxed);

  // Open the shared wake channel if not already open.  For static
  // thread_local state the eventfd is opened once and reused across
  // tests; for stack-allocated state it is opened per test and
  // cleaned up by ~wake_channel().  This mirrors how production
  // io_context opens the channel in its constructor.
  if (!global_tasks.wake_channel_.is_open()) {
    (void)global_tasks.wake_channel_.open();
  }
}

inline bool queue_init_result_or_skip(io_uring_context& context,
                                      io_uring_context_options options) {
  const int result = context.queue_init(options);
  if (result < 0) {
    EXPECT_TRUE(is_unsupported_ring_error(result));
    return false;
  }
  return true;
}

inline bool queue_init_with_state_or_skip(
    io_uring_context& context, io_uring_task_queue_state& global_tasks,
    io_uring_context_options options) {
  reset_task_queue_state(global_tasks);
  context.set_global_state(&global_tasks);
  return queue_init_result_or_skip(context, options);
}

inline bool queue_init_or_skip(io_uring_context& context,
                               io_uring_context_options options = {}) {
  // The state outlives each context run while keeping standalone tests terse.
  static thread_local io_uring_task_queue_state global_tasks;
  return queue_init_with_state_or_skip(context, global_tasks, options);
}

inline bool queue_init_shared_or_skip(io_uring_context& context,
                                      io_uring_task_queue_state& global_tasks,
                                      io_uring_context_options options = {}) {
  return queue_init_with_state_or_skip(context, global_tasks, options);
}

}  // namespace bnio_async_io_io_uring_test

#endif  // BNIO_TESTS_ASYNC_IO_IO_URING_CONTEXT_TEST_SUPPORT_H_
