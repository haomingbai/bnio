/**
 * @file write_all.h
 * @brief write-all composite async operation.
 */

#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_WRITE_ALL_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_POSIX_IO_CONTEXT_WRITE_ALL_H_

#include <bexec/detail/manual_lifetime.hpp>
#include <bexec/just.hpp>
#include <bexec/let.hpp>
#include <bexec/repeat_until.hpp>

namespace bnio::detail {

// Adaptive eager I/O control attached to each read-all/write-all step native
// I/O operation. Consulted by native_io_operation::start() to decide whether
// to probe for immediate completion. The eager flag is rewritten by the step
// completion handler based on whether the previous step filled its buffer
// (bytes == expected).
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
    return native_io_sender(
        *context, make_stream_write_request(socket, current_buffer(), flags),
        adaptive_eager_control<socket_write_all_state>{this});
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
  // Adaptive eager probing: cleared when the previous step had a short
  // transfer, so the next step skips the immediate-completion probe.
  bool eager = true;
};

}  // namespace bnio::detail

#include <bnio/detail/posix/io_context/random_access_write_all.h>
#include <bnio/detail/posix/io_context/stream_file_write_all.h>

namespace bnio::detail {

template <class State>
[[nodiscard]] auto write_all_step_complete(State* state, std::error_code ec,
                                           std::size_t bytes) noexcept {
  if (ec) {
    // Failure/cancellation: terminate. Must set done=true so repeat_until's
    // predicate exits the loop; otherwise the same failed operation would be
    // retried forever.
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
      return bexec::just(std::error_code{}, state->transferred);
    }
  }

  state->advance(bytes);
  return bexec::just(std::error_code{}, state->transferred);
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
    // The env type depends only on the operation's Receiver template
    // parameter; naming it explicitly keeps get_env's return type available
    // while the operation class is still incomplete (child operation types
    // are computed inside its own definition). A deduced decltype(auto)
    // return would force the body — and its operation_->receiver_ access —
    // to be instantiated too early.
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
      // set_stopped now exclusively means the receiver's stop token was
      // observed cancelled (repeat_until::drain() checks it before each
      // round), so it is forwarded unconditionally. io_context::stop()
      // aborts without token cancellation complete the step with
      // set_value(operation_canceled, transferred) instead and never reach
      // this receiver as a token/context distinction.
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
      // Stop token already requested before start: deliver set_stopped
      // (token cancellation wins over value delivery).
      complete_stopped();
      return;
    }
    if (state_.empty()) {
      // Empty buffer: succeed with 0 bytes
      complete_value(std::error_code{}, state_.transferred);
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
