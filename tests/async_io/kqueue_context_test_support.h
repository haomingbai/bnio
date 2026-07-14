#pragma once
#ifndef BUPP_TESTS_ASYNC_IO_KQUEUE_CONTEXT_TEST_SUPPORT_H_
#define BUPP_TESTS_ASYNC_IO_KQUEUE_CONTEXT_TEST_SUPPORT_H_

#include <bupp/async_io/bsd/kqueue_context.h>

#include <bexec/stop_token.hpp>
#include <memory>
#include <system_error>

namespace bupp_async_io_kqueue_test {

using bupp::async_io::buffer_view;
using bupp::async_io::descriptor_view;
using bupp::async_io::bsd_native::kqueue_context;
using bupp::async_io::bsd_native::kqueue_context_options;
using bupp::async_io::bsd_native::kqueue_helper;
using bupp::async_io::bsd_native::kqueue_io_operation_base;
using bupp::async_io::bsd_native::kqueue_nop_operation;
using bupp::async_io::bsd_native::kqueue_operation_base;
using bupp::async_io::bsd_native::kqueue_poll_operation;
using bupp::async_io::bsd_native::kqueue_post_operation;
using bupp::async_io::bsd_native::kqueue_raw_operation;
using bupp::async_io::bsd_native::kqueue_read_operation;
using bupp::async_io::bsd_native::kqueue_task_queue_state;
using bupp::async_io::bsd_native::kqueue_write_operation;

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
  kqueue_context* context = nullptr;
  bool stop_on_completion = false;

  void set_value() noexcept {
    state->signal = signal_kind::value;
    complete();
  }

  void set_value(int result, unsigned flags) noexcept {
    state->signal = signal_kind::value;
    state->result = result;
    state->flags = flags;
    complete();
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    complete();
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    complete();
  }

 private:
  void complete() noexcept {
    state->in_context = context != nullptr && context->is_in_context();
    if (stop_on_completion && context != nullptr) {
      (void)context->stop();
    }
  }
};

struct poll_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  kqueue_context* context = nullptr;

  void set_value(unsigned events) noexcept {
    state->signal = signal_kind::value;
    state->result = static_cast<int>(events);
    complete();
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    complete();
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    complete();
  }

 private:
  void complete() noexcept {
    state->in_context = context != nullptr && context->is_in_context();
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
  stop_env environment;

  [[nodiscard]] stop_env get_env() const noexcept { return environment; }
};

}  // namespace bupp_async_io_kqueue_test

#endif  // BUPP_TESTS_ASYNC_IO_KQUEUE_CONTEXT_TEST_SUPPORT_H_
