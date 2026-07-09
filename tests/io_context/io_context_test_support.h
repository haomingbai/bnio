#include <bupp/io_context.h>
#include <bupp/tcp.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <bexec/operation_state.hpp>
#include <bexec/scheduler.hpp>
#include <bexec/sender.hpp>
#include <bexec/stop_token.hpp>
#include <cassert>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

enum class signal_kind {
  none,
  value,
  error,
  stopped,
};

struct shared_state {
  signal_kind signal = signal_kind::none;
  std::size_t size = 0;
  int fd = -1;
  std::error_code error;
};

struct schedule_state {
  signal_kind signal = signal_kind::none;
  std::vector<int> order;
  unsigned completions = 0;
  bool completed_during_start = false;
};

struct byte_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bupp::io_context* context = nullptr;

  void set_value(std::size_t size) noexcept {
    state->signal = signal_kind::value;
    state->size = size;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct socket_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bupp::io_context* context = nullptr;
  unsigned* completions = nullptr;
  unsigned target = 1;

  void set_value(bupp::tcp_socket socket) noexcept {
    state->signal = signal_kind::value;
    state->fd = socket.release();
    if (completions != nullptr) {
      ++*completions;
      if (*completions != target) {
        return;
      }
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    if (completions != nullptr) {
      ++*completions;
      if (*completions != target) {
        return;
      }
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct void_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bupp::io_context* context = nullptr;
  unsigned* completions = nullptr;
  unsigned target = 1;

  void set_value() noexcept {
    state->signal = signal_kind::value;
    if (completions != nullptr) {
      ++*completions;
      if (*completions != target) {
        return;
      }
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    if (completions != nullptr) {
      ++*completions;
      if (*completions != target) {
        return;
      }
    }
    if (context != nullptr) {
      (void)context->stop();
    }
  }
};

struct poll_receiver {
  std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
  bupp::io_context* context = nullptr;

  void set_value(unsigned events) noexcept {
    state->signal = signal_kind::value;
    state->size = events;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    state->signal = signal_kind::error;
    state->error = error;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
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

struct stopped_void_receiver : void_receiver {
  stop_env env;

  [[nodiscard]] stop_env get_env() const noexcept { return env; }
};

struct schedule_receiver {
  std::shared_ptr<schedule_state> state = std::make_shared<schedule_state>();
  bupp::io_context* context = nullptr;
  int value = 0;
  unsigned target = 1;

  void set_value() noexcept {
    state->signal = signal_kind::value;
    state->order.push_back(value);
    ++state->completions;
    if (context != nullptr && state->completions == target) {
      (void)context->stop();
    }
  }

  void set_error(std::error_code error) noexcept {
    (void)error;
    state->signal = signal_kind::error;
    if (context != nullptr) {
      (void)context->stop();
    }
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    ++state->completions;
    if (context != nullptr && state->completions == target) {
      (void)context->stop();
    }
  }
};

struct stopped_schedule_receiver : schedule_receiver {
  stop_env env;

  [[nodiscard]] stop_env get_env() const noexcept { return env; }
};

struct dispatch_inline_outer_receiver {
  std::shared_ptr<schedule_state> state;
  bupp::io_context* context = nullptr;

  void set_value() noexcept {
    schedule_receiver inner;
    inner.state = state;
    inner.value = 42;

    auto sender = bexec::schedule(context->get_dispatch_scheduler());
    auto operation = bexec::connect(std::move(sender), std::move(inner));
    bexec::start(operation);

    state->completed_during_start = state->signal == signal_kind::value;
    (void)context->stop();
  }

  void set_error(std::error_code error) noexcept {
    (void)error;
    state->signal = signal_kind::error;
    (void)context->stop();
  }

  void set_stopped() noexcept {
    state->signal = signal_kind::stopped;
    (void)context->stop();
  }
};

template <class Scheduler, class Stream, class Buffer>
concept scheduler_can_read_stream =
    requires(Scheduler scheduler, Stream stream, Buffer buffer) {
      scheduler.async_read(stream, buffer);
    };

template <class Scheduler, class Stream, class Buffer>
concept scheduler_can_write_stream =
    requires(Scheduler scheduler, Stream stream, Buffer buffer) {
      scheduler.async_write(stream, buffer);
    };

}  // namespace
