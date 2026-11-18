/**
 * @file step.h
 * @brief SSL read/write operation state machine steps.
 */

#pragma once
#ifndef BNIO_SSL_DETAIL_READ_WRITE_STEP_H_
#define BNIO_SSL_DETAIL_READ_WRITE_STEP_H_

#include <bnio/ssl/base/errors.h>
#include <bnio/ssl/detail/read_write/state.h>

#include <atomic>
#include <bexec/completion_signatures.hpp>
#include <bexec/detail/operation_storage.hpp>
#include <bexec/receiver.hpp>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::ssl {

/** @cond BNIO_DETAIL */
namespace detail {

template <class State>
class step_sender {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  explicit step_sender(State* state) noexcept : state_(state) {}

  template <class Receiver>
  auto connect(Receiver receiver) const noexcept {
    return step_operation<State, std::remove_cvref_t<Receiver>>(
        state_, std::move(receiver));
  }

 private:
  State* state_;
};

template <class State>
class step_factory {
 public:
  explicit step_factory(State* state) noexcept : state_(state) {}

  [[nodiscard]] auto operator()() const noexcept {
    return step_sender<State>(state_);
  }

 private:
  State* state_;
};

template <class State>
class done_predicate {
 public:
  explicit done_predicate(State* state) noexcept : state_(state) {}

  [[nodiscard]] bool operator()() const noexcept {
    std::atomic_thread_fence(std::memory_order_acquire);
    return state_->done;
  }

 private:
  State* state_;
};

template <class State, class Receiver>
class step_operation {
 public:
  using receiver_type = std::remove_cvref_t<Receiver>;

  class child_receiver {
   public:
    // Named explicitly so get_env's return type is available while the
    // operation class is incomplete.
    using env_type = decltype(bexec::get_env(std::declval<receiver_type&>()));

    explicit child_receiver(step_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] env_type get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::error_code ec, std::size_t bytes) noexcept {
      operation_->handle_transport_complete(ec, bytes);
    }

    void set_stopped() noexcept { operation_->complete_stopped(); }

   private:
    step_operation* operation_;
  };

  using read_sender_type = decltype(make_transport_read_sender(
      std::declval<typename State::scheduler_type&>(),
      std::declval<tcp::stream<typename State::next_layer_type>&>(),
      static_cast<void*>(nullptr), std::size_t{}));
  using write_sender_type = decltype(make_transport_write_sender(
      std::declval<typename State::scheduler_type&>(),
      std::declval<tcp::stream<typename State::next_layer_type>&>(),
      static_cast<const void*>(nullptr), std::size_t{}));
  using read_operation_type = decltype(bexec::connect(
      std::declval<read_sender_type>(), std::declval<child_receiver>()));
  using write_operation_type = decltype(bexec::connect(
      std::declval<write_sender_type>(), std::declval<child_receiver>()));
  using child_operations_type = bexec::detail::operation_storage<
      bexec::type_list<read_operation_type, write_operation_type>>;

  step_operation(State* state, Receiver receiver)
      : state_(state), receiver_(std::move(receiver)) {}

  void start() noexcept {
    if (stop_requested(receiver_)) {
      // Token canceled at start: deliver set_stopped. Context-stop aborts
      // surface as value(operation_canceled, bytes) from transport arbitration.
      complete_stopped();
      return;
    }

    run_step();
  }

 private:
  void run_step() noexcept {
    switch (state_->phase) {
      case io_phase::application:
        run_application();
        return;
      case io_phase::flush_output:
        flush_output();
        return;
      case io_phase::transport_read:
        submit_transport_read();
        return;
      case io_phase::transport_write:
        submit_transport_write();
        return;
      case io_phase::done:
        complete_value(base::empty_error_code, state_->bytes);
        return;
    }
  }

  void run_application() noexcept {
    base::clear_errors();
    if constexpr (State::application == application_io::read) {
      void* data = state_->buffer.data();
      const int result = SSL_read(native_ssl(*state_->stream), data,
                                  bounded_int_size(state_->buffer.size()));
      handle_application_result(result);
    } else {
      const auto* data = static_cast<const char*>(state_->buffer.data());
      const std::size_t remaining = state_->buffer.size() - state_->bytes;
      const int result =
          SSL_write(native_ssl(*state_->stream), data + state_->bytes,
                    bounded_int_size(remaining));
      handle_application_result(result);
    }
  }

  void handle_application_result(int result) noexcept {
    if (result > 0) {
      const std::size_t transferred = static_cast<std::size_t>(result);
      if constexpr (State::application == application_io::read) {
        state_->bytes = transferred;
      } else {
        if (transferred > state_->buffer.size() - state_->bytes) {
          complete_error(std::make_error_code(std::errc::protocol_error));
          return;
        }
        state_->bytes += transferred;
      }
      state_->after_flush = resume_action::finish;
      if constexpr (State::application == application_io::write &&
                    State::complete_buffer) {
        if (state_->bytes < state_->buffer.size()) {
          state_->after_flush = State::application_action;
        }
      }
      state_->phase = io_phase::flush_output;
      complete_value(base::empty_error_code, 0);
      return;
    }

    handle_ssl_error(result);
  }

  void handle_ssl_error(int ssl_result) noexcept {
    const int error = SSL_get_error(native_ssl(*state_->stream), ssl_result);
    switch (error) {
      case SSL_ERROR_WANT_READ:
        state_->after_flush = resume_action::transport_read;
        state_->phase = io_phase::flush_output;
        complete_value(base::empty_error_code, 0);
        return;
      case SSL_ERROR_WANT_WRITE:
        state_->after_flush = State::application_action;
        state_->phase = io_phase::flush_output;
        complete_value(base::empty_error_code, 0);
        return;
      case SSL_ERROR_ZERO_RETURN:
        state_->done = true;
        if constexpr (State::application == application_io::read) {
          // Orderly TLS close (close_notify): complete the read with the bytes
          // so far, the same EOF encoding as a plain socket read.
          complete_value(base::empty_error_code, state_->bytes);
        } else {
          // An orderly close mid-write: report broken_pipe with the bytes so
          // far, never an empty ec, which would mask a short write-all.
          complete_value(std::make_error_code(std::errc::broken_pipe),
                         state_->bytes);
        }
        return;
      default:
        state_->done = true;
        complete_value(base::last_error(), state_->bytes);
        return;
    }
  }

  void flush_output() noexcept {
    char* data = nullptr;
    const int available = BIO_nread0(write_bio(*state_->stream), &data);
    if (available > 0) {
      state_->transport_data = data;
      state_->transport_size = static_cast<std::size_t>(available);
      state_->phase = io_phase::transport_write;
      submit_transport_write();
      return;
    }

    resume_after_flush();
  }

  void resume_after_flush() noexcept {
    switch (state_->after_flush) {
      case resume_action::transport_read:
        state_->phase = io_phase::transport_read;
        submit_transport_read();
        return;
      case resume_action::finish:
        state_->phase = io_phase::done;
        state_->done = true;
        std::atomic_thread_fence(std::memory_order_release);
        complete_value(base::empty_error_code, state_->bytes);
        return;
      case resume_action::application_read:
      case resume_action::application_write:
        state_->phase = io_phase::application;
        complete_value(base::empty_error_code, 0);
        return;
      default:
        return;
    }
  }

  void submit_transport_read() noexcept {
    char* data = nullptr;
    base::clear_errors();
    const int available = BIO_nwrite0(read_bio(*state_->stream), &data);
    if (available <= 0) {
      state_->done = true;
      complete_value(base::last_error(), state_->bytes);
      return;
    }

    state_->transport_data = data;
    state_->transport_size = static_cast<std::size_t>(available);
    child_operation_.template emplace_from<read_operation_type>([this] {
      return bexec::connect(make_transport_read_sender(
                                state_->scheduler, *state_->stream,
                                state_->transport_data, state_->transport_size),
                            child_receiver(*this));
    });
    child_operation_.start();
  }

  void submit_transport_write() noexcept {
    child_operation_.template emplace_from<write_operation_type>([this] {
      return bexec::connect(make_transport_write_sender(
                                state_->scheduler, *state_->stream,
                                state_->transport_data, state_->transport_size),
                            child_receiver(*this));
    });
    child_operation_.start();
  }

  void handle_transport_complete(std::error_code ec,
                                 std::size_t bytes) noexcept {
    if (ec) {
      state_->done = true;
      complete_value(ec, state_->bytes);
      return;
    }
    if (bytes == 0) {
      // EOF: stop the loop (SSL_read keeps returning WANT_READ and the
      // transport keeps returning 0), otherwise repeat_until never exits.
      state_->done = true;
      if (state_->phase == io_phase::transport_write) {
        // 0-byte transport write mid-write-all: broken_pipe, as for TCP.
        complete_value(std::make_error_code(std::errc::broken_pipe),
                       state_->bytes);
        return;
      }
      // 0-byte transport read: peer closed without close_notify, a truncated
      // stream reported as connection_reset.
      complete_value(std::make_error_code(std::errc::connection_reset),
                     state_->bytes);
      return;
    }
    switch (state_->phase) {
      case io_phase::transport_read:
        handle_transport_read_complete(bytes);
        return;
      case io_phase::transport_write:
        handle_transport_write_complete(bytes);
        return;
      case io_phase::application:
      case io_phase::flush_output:
      case io_phase::done:
        complete_error(std::make_error_code(std::errc::protocol_error));
        return;
    }
  }

  void handle_transport_read_complete(std::size_t result) noexcept {
    // result > 0: non-positive results were already filtered.

    char* data = nullptr;
    base::clear_errors();
    const int committed =
        BIO_nwrite(read_bio(*state_->stream), &data, bounded_int_size(result));
    if (committed != static_cast<int>(result)) {
      complete_error(base::last_error());  // Invariant violation: passed
                                           // through set_value(ec, bytes)
      return;
    }

    state_->phase = io_phase::application;
    complete_value(base::empty_error_code, 0);
  }

  void handle_transport_write_complete(std::size_t result) noexcept {
    // result > 0: non-positive results were already filtered.

    char* data = nullptr;
    base::clear_errors();
    const int consumed =
        BIO_nread(write_bio(*state_->stream), &data, bounded_int_size(result));
    if (consumed != static_cast<int>(result)) {
      complete_error(base::last_error());  // Invariant violation: passed
                                           // through set_value(ec, bytes)
      return;
    }

    state_->phase = io_phase::flush_output;
    complete_value(base::empty_error_code, 0);
  }

  void complete_value(std::error_code ec, std::size_t bytes) noexcept {
    bexec::set_value(std::move(receiver_), ec, bytes);
  }

  void complete_error(std::error_code error) noexcept {
    // Passed through set_value(ec, bytes) so repeat_receiver takes the
    // complete_value path; done=true ensures repeat_until exits.
    state_->done = true;
    bexec::set_value(std::move(receiver_), error, state_->bytes);
  }

  void complete_stopped() noexcept { bexec::set_stopped(std::move(receiver_)); }

  State* state_;
  receiver_type receiver_;
  child_operations_type child_operation_;
};

}  // namespace detail
/** @endcond */

}  // namespace bnio::ssl

#endif  // BNIO_SSL_DETAIL_READ_WRITE_STEP_H_
