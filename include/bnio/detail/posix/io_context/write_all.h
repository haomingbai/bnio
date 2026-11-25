/**
 * @file write_all.h
 * @brief write-all composite async operation.
 */

#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_WRITE_ALL_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_POSIX_IO_CONTEXT_WRITE_ALL_H_

#include <bnio/detail/error_code.h>

#include <bexec/detail/manual_lifetime.hpp>
#include <bexec/just.hpp>
#include <bexec/let.hpp>
#include <bexec/repeat_until.hpp>

namespace bnio::detail {

// Decides whether each read-all/write-all step probes for immediate
// completion; the step handler sets eager to (bytes == expected), so a short
// transfer disables the next probe.
template <class State>
class adaptive_eager_control {
 public:
  explicit adaptive_eager_control(State* state) noexcept : state_(state) {}
  [[nodiscard]] bool operator()() const noexcept {
    return state_->context->enable_immediate_io() && state_->eager;
  }

 private:
  State* state_;
};

template <io_context::schedule_kind Kind>
class socket_write_all_state {
 public:
  static constexpr bool zero_byte_is_error = true;

  socket_write_all_state(io_context& context,
                         async_io::stream_socket_view socket,
                         const_buffer buffer, int flags) noexcept
      : context(&context), socket(socket), buffer(buffer), flags(flags) {}

  [[nodiscard]] std::size_t remaining() const noexcept {
    return buffer.size() - transferred;
  }

  [[nodiscard]] bool empty() const noexcept { return buffer.size() == 0; }

  [[nodiscard]] const_buffer current_buffer() const noexcept {
    const auto* data = static_cast<const char*>(buffer.data());
    return const_buffer(data + transferred, remaining());
  }

  [[nodiscard]] auto make_sender() noexcept {
    return make_io_sender<Kind>(
        *context, make_stream_write_request(socket, current_buffer(), flags),
        adaptive_eager_control<socket_write_all_state<Kind> >{this});
  }

  void advance(std::size_t bytes) noexcept {
    transferred += bytes;
    if (transferred >= buffer.size()) {
      done = true;
    }
  }

  io_context* context;
  async_io::stream_socket_view socket;
  const_buffer buffer;
  int flags;
  std::size_t transferred = 0;
  bool done = false;
  // Set to (bytes == expected) by the step handler; a short transfer
  // disables the next probe.
  bool eager = true;
};

// Local (AF_UNIX) twin of socket_write_all_state: the step operation goes
// through the local write factory, so the socket type selects the model.
template <io_context::schedule_kind Kind>
class local_socket_write_all_state {
 public:
  static constexpr bool zero_byte_is_error = true;

  local_socket_write_all_state(io_context& context,
                               async_io::local::stream_socket_view socket,
                               const_buffer buffer, int flags) noexcept
      : context(&context), socket(socket), buffer(buffer), flags(flags) {}

  [[nodiscard]] std::size_t remaining() const noexcept {
    return buffer.size() - transferred;
  }

  [[nodiscard]] bool empty() const noexcept { return buffer.size() == 0; }

  [[nodiscard]] const_buffer current_buffer() const noexcept {
    const auto* data = static_cast<const char*>(buffer.data());
    return const_buffer(data + transferred, remaining());
  }

  [[nodiscard]] auto make_sender() noexcept {
    return make_io_sender<Kind>(
        *context,
        make_local_stream_write_request(socket, current_buffer(), flags),
        adaptive_eager_control<local_socket_write_all_state<Kind> >{this});
  }

  void advance(std::size_t bytes) noexcept {
    transferred += bytes;
    if (transferred >= buffer.size()) {
      done = true;
    }
  }

  io_context* context;
  async_io::local::stream_socket_view socket;
  const_buffer buffer;
  int flags;
  std::size_t transferred = 0;
  bool done = false;
  // Set to (bytes == expected) by the step handler; a short transfer
  // disables the next probe.
  bool eager = true;
};

}  // namespace bnio::detail

#include <bnio/detail/posix/io_context/random_access_write_all.h>
#include <bnio/detail/posix/io_context/stream_file_write_all.h>

namespace bnio::detail {

// Default: report the backend's errno unchanged.
template <class State>
[[nodiscard]] std::error_code normalize_step_error(
    const State&, std::error_code ec) noexcept {
  return ec;
}

// XNU/FreeBSD uipc_send checks SS_ISCONNECTED before SS_CANTSENDMORE, so an
// AF_UNIX send racing the peer's close can return ENOTCONN. Bytes already
// transferred prove the stream was connected: that is a lost peer and must
// read as EPIPE. At zero progress ENOTCONN still means "not connected" and
// passes through.
template <io_context::schedule_kind Kind>
[[nodiscard]] std::error_code normalize_step_error(
    const local_socket_write_all_state<Kind>& state,
    std::error_code ec) noexcept {
  if (ec == std::errc::not_connected && state.transferred > 0) {
    return std::make_error_code(std::errc::broken_pipe);
  }
  return ec;
}

template <class State>
[[nodiscard]] auto write_all_step_complete(State* state, std::error_code ec,
                                           std::size_t bytes) noexcept {
  ec = normalize_step_error(*state, ec);
  if (ec) {
    // Failure/cancellation: set done so repeat_until exits instead of
    // retrying the same failed operation forever.
    state->done = true;
    return bexec::just(ec, state->transferred);
  }
  if (bytes == 0) {
    // Zero-byte transfer: for write-all this is an error (peer closed /
    // broken pipe); for read-all it is EOF, which terminates successfully.
    state->done = true;
    if constexpr (State::zero_byte_is_error) {
      return bexec::just(std::make_error_code(std::errc::broken_pipe),
                         state->transferred);
    } else {
      return bexec::just(bnio::detail::empty_error_code, state->transferred);
    }
  }

  state->advance(bytes);
  return bexec::just(bnio::detail::empty_error_code, state->transferred);
}

template <class State>
class write_all_step_factory {
 public:
  explicit write_all_step_factory(State* state) noexcept : state_(state) {}

  [[nodiscard]] auto operator()() const noexcept {
    const std::size_t expected = state_->remaining();
    return bexec::let_value(
        state_->make_sender(),
        [state = state_, expected](std::error_code ec,
                                   std::size_t bytes) noexcept {
          state->eager = (bytes == expected);
          return write_all_step_complete(state, ec, bytes);
        });
  }

 private:
  State* state_;
};

template <class State>
class write_all_done_predicate {
 public:
  explicit write_all_done_predicate(State* state) noexcept : state_(state) {}

  [[nodiscard]] bool operator()() const noexcept { return state_->done; }

 private:
  State* state_;
};

template <class State, class Receiver>
class write_all_operation {
 public:
  using receiver_type = std::remove_cvref_t<Receiver>;
  using factory_type = write_all_step_factory<State>;
  using predicate_type = write_all_done_predicate<State>;
  using repeat_sender_type = decltype(bexec::repeat_until(
      std::declval<factory_type>(), std::declval<predicate_type>()));

  class repeat_receiver {
   public:
    // env_type is named so get_env's type is available while the operation
    // is incomplete; a deduced return would instantiate the body too early.
    using env_type = decltype(bexec::get_env(std::declval<receiver_type&>()));

    explicit repeat_receiver(write_all_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] env_type get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::error_code ec, std::size_t bytes_written) noexcept {
      operation_->complete_value(ec, bytes_written);
    }

    void set_stopped() noexcept {
      // set_stopped means the stop token was cancelled; io_context::stop()
      // instead completes with set_value(operation_canceled, transferred).
      operation_->complete_stopped();
    }

   private:
    write_all_operation* operation_;
  };

  using repeat_operation_type = decltype(bexec::connect(
      std::declval<repeat_sender_type>(), std::declval<repeat_receiver>()));

  write_all_operation(State state, Receiver receiver)
      : state_(std::move(state)), receiver_(std::move(receiver)) {
    repeat_operation_.emplace_from([this] {
      return bexec::connect(
          bexec::repeat_until(factory_type(&state_), predicate_type(&state_)),
          repeat_receiver(*this));
    });
  }

  write_all_operation(const write_all_operation&) = delete;
  write_all_operation& operator=(const write_all_operation&) = delete;
  write_all_operation(write_all_operation&&) = delete;
  write_all_operation& operator=(write_all_operation&&) = delete;

  void start() noexcept {
    if (stop_requested(receiver_)) {
      // Stop token already cancelled: set_stopped wins.
      complete_stopped();
      return;
    }
    if (state_.empty()) {
      // Empty buffer: succeed with 0 bytes
      complete_value(bnio::detail::empty_error_code, state_.transferred);
      return;
    }

    bexec::start(*repeat_operation_);
  }

 private:
  void complete_value(std::error_code ec, std::size_t bytes_written) noexcept {
    bexec::set_value(std::move(receiver_), ec, bytes_written);
  }

  void complete_stopped() noexcept { bexec::set_stopped(std::move(receiver_)); }

  State state_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<repeat_operation_type> repeat_operation_;
};

template <class State>
class write_all_sender {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  explicit write_all_sender(State state) noexcept : state_(std::move(state)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return write_all_operation<State, std::remove_cvref_t<Receiver> >(
        std::move(state_), std::move(receiver));
  }

  template <class Receiver>
  auto connect(Receiver receiver) const& {
    return write_all_operation<State, std::remove_cvref_t<Receiver> >(
        state_, std::move(receiver));
  }

 private:
  State state_;
};

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_WRITE_ALL_H_
