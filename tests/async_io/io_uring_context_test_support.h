#pragma once
#ifndef BUPP_TESTS_ASYNC_IO_IO_URING_CONTEXT_TEST_SUPPORT_H_
#define BUPP_TESTS_ASYNC_IO_IO_URING_CONTEXT_TEST_SUPPORT_H_

#include <bupp/async_io/linux/io_uring_context.h>
#include <bupp/base/linux/liburing.h>
#include <fcntl.h>
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
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

namespace bupp_async_io_io_uring_test {

using bupp::async_io::buffer_view;
using bupp::async_io::descriptor_view;
using bupp::async_io::listening_socket_view;
using bupp::async_io::stream_socket_view;
using bupp::async_io::linux_native::buffer_sequence_view;
using bupp::async_io::linux_native::const_message_view;
using bupp::async_io::linux_native::io_uring_accept_operation;
using bupp::async_io::linux_native::io_uring_connect_operation;
using bupp::async_io::linux_native::io_uring_context;
using bupp::async_io::linux_native::io_uring_context_options;
using bupp::async_io::linux_native::io_uring_nop_operation;
using bupp::async_io::linux_native::io_uring_operation_base;
using bupp::async_io::linux_native::io_uring_poll_operation;
using bupp::async_io::linux_native::io_uring_post_operation;
using bupp::async_io::linux_native::io_uring_read_operation;
using bupp::async_io::linux_native::io_uring_readv_operation;
using bupp::async_io::linux_native::io_uring_recv_operation;
using bupp::async_io::linux_native::io_uring_recvmsg_operation;
using bupp::async_io::linux_native::io_uring_resolve_operation;
using bupp::async_io::linux_native::io_uring_send_operation;
using bupp::async_io::linux_native::io_uring_sendmsg_operation;
using bupp::async_io::linux_native::io_uring_timeout_operation;
using bupp::async_io::linux_native::io_uring_write_operation;
using bupp::async_io::linux_native::io_uring_writev_operation;
using bupp::async_io::linux_native::mutable_message_view;

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
  int prepare_result = -1;
};

struct receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  io_uring_context* context = nullptr;
  bool stop_on_completion = false;

  void set_value() noexcept {
    state->signal = signal_kind::value;
    state->in_context = (context != nullptr && context->is_in_context());
    if (stop_on_completion && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_value(int result, unsigned flags) noexcept {
    state->signal = signal_kind::value;
    state->result = result;
    state->flags = flags;
    state->in_context = (context != nullptr && context->is_in_context());
    if (stop_on_completion && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    state->in_context = (context != nullptr && context->is_in_context());
    if (stop_on_completion && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
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

  void set_value(unsigned events) noexcept {
    state->signal = signal_kind::value;
    state->result = static_cast<int>(events);
    state->in_context = (context != nullptr && context->is_in_context());
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    state->in_context = (context != nullptr && context->is_in_context());
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
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

  void set_value(std::size_t count) noexcept {
    if (state != nullptr) {
      state->signal = signal_kind::value;
      state->endpoint_count = count;
      state->in_context = (context != nullptr && context->is_in_context());
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    if (state != nullptr) {
      state->signal = signal_kind::error;
      state->error = error;
      state->in_context = (context != nullptr && context->is_in_context());
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
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

struct fill_nop_operation : io_uring_operation_base {
  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_nop();
  }

  void execute() noexcept override {}
};

struct prepare_on_completion_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  io_uring_context* context = nullptr;
  fill_nop_operation* filler = nullptr;

  void set_value(int result, unsigned flags) noexcept {
    state->signal = signal_kind::value;
    state->result = result;
    state->flags = flags;
    state->in_context = (context != nullptr && context->is_in_context());
    if (context != nullptr && filler != nullptr) {
      state->prepare_result = context->prepare(*filler);
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    state->in_context = (context != nullptr && context->is_in_context());
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    state->in_context = (context != nullptr && context->is_in_context());
    if (context != nullptr) {
      (void)context->stop();
    }
  }
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

  void set_value(int result, unsigned /*flags*/) noexcept {
    assert(result == 0);
    ++state->completed;
    state->all_in_context = state->all_in_context &&
                            (context != nullptr && context->is_in_context());
    if (state->completed == target && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code /*error*/) noexcept {
    ++state->errors;
    state->all_in_context = state->all_in_context &&
                            (context != nullptr && context->is_in_context());
    if (context != nullptr) {
      (void)context->stop();
    }
  }

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

  void set_value() noexcept {
    state->in_order = state->in_order && index == state->next_index;
    ++state->next_index;
    ++state->completed;
    state->all_in_context = state->all_in_context &&
                            (context != nullptr && context->is_in_context());
    if (state->completed == target && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code /*error*/) noexcept {
    ++state->errors;
    state->all_in_context = state->all_in_context &&
                            (context != nullptr && context->is_in_context());
    if (context != nullptr) {
      (void)context->stop();
    }
  }

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

  void set_value(int result, unsigned /*flags*/) noexcept {
    assert(result == 0);
    const unsigned completed =
        state->completed.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (completed == target && context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code /*error*/) noexcept {
    state->errors.fetch_add(1, std::memory_order_acq_rel);
    if (context != nullptr) {
      (void)context->stop();
    }
  }

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

inline bool queue_init_or_skip(io_uring_context& context,
                               io_uring_context_options options = {}) {
  const int result = context.queue_init(options);
  if (result < 0) {
    assert(is_unsupported_ring_error(result));
    return false;
  }
  return true;
}

}  // namespace bupp_async_io_io_uring_test

#endif  // BUPP_TESTS_ASYNC_IO_IO_URING_CONTEXT_TEST_SUPPORT_H_
