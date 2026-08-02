/**
 * @file step.h
 * @brief SSL read/write operation state machine steps.
 */

#pragma once
#ifndef BNIO_DETAIL_SSL_ASYNC_OPERATIONS_READ_WRITE_STEP_H_
#define BNIO_DETAIL_SSL_ASYNC_OPERATIONS_READ_WRITE_STEP_H_

#include <bnio/detail/ssl/async_operations/read_write/state.h>

#include <atomic>
#include <bexec/completion_signatures.hpp>
#include <bexec/detail/operation_storage.hpp>
#include <bexec/receiver.hpp>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio {

/** @cond BNIO_DETAIL */
namespace detail {

template <class State>
class ssl_io_step_sender {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  explicit ssl_io_step_sender(State* state) noexcept : state_(state) {}

  template <class Receiver>
  auto connect(Receiver receiver) const {
    return ssl_io_step_operation<State, std::remove_cvref_t<Receiver>>(
        state_, std::move(receiver));
  }

 private:
  State* state_;
};

template <class State>
class ssl_io_step_factory {
 public:
  explicit ssl_io_step_factory(State* state) noexcept : state_(state) {}

  [[nodiscard]] auto operator()() const noexcept {
    return ssl_io_step_sender<State>(state_);
  }

 private:
  State* state_;
};

template <class State>
class ssl_io_done_predicate {
 public:
  explicit ssl_io_done_predicate(State* state) noexcept : state_(state) {}

  [[nodiscard]] bool operator()() const noexcept {
    std::atomic_thread_fence(std::memory_order_acquire);
    return state_->done;
  }

 private:
  State* state_;
};

template <class State, class Receiver>
class ssl_io_step_operation {
 public:
  using receiver_type = std::remove_cvref_t<Receiver>;

  class child_receiver {
   public:
    explicit child_receiver(ssl_io_step_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] decltype(auto) get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::error_code ec, std::size_t bytes) noexcept {
      operation_->handle_transport_complete(ec, bytes);
    }

    void set_stopped() noexcept { operation_->complete_stopped(); }

   private:
    ssl_io_step_operation* operation_;
  };

  using read_sender_type = decltype(ssl_make_transport_read_sender(
      std::declval<typename State::scheduler_type&>(),
      std::declval<ssl_stream<typename State::next_layer_type>&>(),
      static_cast<void*>(nullptr), std::size_t{}));
  using write_sender_type = decltype(ssl_make_transport_write_sender(
      std::declval<typename State::scheduler_type&>(),
      std::declval<ssl_stream<typename State::next_layer_type>&>(),
      static_cast<const void*>(nullptr), std::size_t{}));
  using read_operation_type = decltype(bexec::connect(
      std::declval<read_sender_type>(), std::declval<child_receiver>()));
  using write_operation_type = decltype(bexec::connect(
      std::declval<write_sender_type>(), std::declval<child_receiver>()));
  using child_operations_type = bexec::detail::operation_storage<
      bexec::type_list<read_operation_type, write_operation_type>>;

  ssl_io_step_operation(State* state, Receiver receiver)
      : state_(state), receiver_(std::move(receiver)) {}

  void start() noexcept {
    if (ssl_stop_requested(receiver_)) {
      // stop-token 取消：通过 set_value(operation_canceled, bytes) 传递，
      // 与 state_machine.h/operation.h/native_io 一致，不走 set_stopped 通道。
      complete_value(std::make_error_code(std::errc::operation_canceled),
                     state_->bytes);
      return;
    }

    run_step();
  }

 private:
  void run_step() noexcept {
    switch (state_->phase) {
      case ssl_io_phase::application:
        run_application();
        return;
      case ssl_io_phase::flush_output:
        flush_output();
        return;
      case ssl_io_phase::transport_read:
        submit_transport_read();
        return;
      case ssl_io_phase::transport_write:
        submit_transport_write();
        return;
      case ssl_io_phase::done:
        complete_value(std::error_code{}, state_->bytes);
        return;
    }
  }

  void run_application() noexcept {
    if constexpr (State::application == ssl_application_io::read) {
      async_io::buffer_view view = state_->buffer.view();
      const int result = SSL_read(state_->stream->native_handle(), view.data,
                                  ssl_bounded_int_size(view.size));
      handle_application_result(result);
    } else {
      const auto* data = static_cast<const char*>(state_->buffer.data());
      const std::size_t remaining = state_->buffer.size() - state_->bytes;
      const int result =
          SSL_write(state_->stream->native_handle(), data + state_->bytes,
                    ssl_bounded_int_size(remaining));
      handle_application_result(result);
    }
  }

  void handle_application_result(int result) noexcept {
    if (result > 0) {
      const std::size_t transferred = static_cast<std::size_t>(result);
      if constexpr (State::application == ssl_application_io::read) {
        state_->bytes = transferred;
      } else {
        if (transferred > state_->buffer.size() - state_->bytes) {
          complete_error(std::make_error_code(std::errc::protocol_error));
          return;
        }
        state_->bytes += transferred;
      }
      state_->after_flush = ssl_resume_action::finish;
      if constexpr (State::application == ssl_application_io::write &&
                    State::complete_buffer) {
        if (state_->bytes < state_->buffer.size()) {
          state_->after_flush = State::application_action;
        }
      }
      state_->phase = ssl_io_phase::flush_output;
      complete_value(std::error_code{}, 0);
      return;
    }

    handle_ssl_error(result);
  }

  void handle_ssl_error(int ssl_result) noexcept {
    const int error =
        SSL_get_error(state_->stream->native_handle(), ssl_result);
    switch (error) {
      case SSL_ERROR_WANT_READ:
        state_->after_flush = ssl_resume_action::transport_read;
        state_->phase = ssl_io_phase::flush_output;
        complete_value(std::error_code{}, 0);
        return;
      case SSL_ERROR_WANT_WRITE:
        state_->after_flush = State::application_action;
        state_->phase = ssl_io_phase::flush_output;
        complete_value(std::error_code{}, 0);
        return;
      case SSL_ERROR_ZERO_RETURN:
        state_->done = true;
        complete_value(std::make_error_code(std::errc::connection_reset),
                       state_->bytes);
        return;
      default:
        state_->done = true;
        complete_value(last_ssl_error(), state_->bytes);
        return;
    }
  }

  void flush_output() noexcept {
    char* data = nullptr;
    const int available = BIO_nread0(write_bio(*state_->stream), &data);
    if (available > 0) {
      state_->transport_data = data;
      state_->transport_size = static_cast<std::size_t>(available);
      state_->phase = ssl_io_phase::transport_write;
      submit_transport_write();
      return;
    }

    if (available < -1) {
      state_->done = true;
      complete_value(last_ssl_error(), state_->bytes);
      return;
    }

    resume_after_flush();
  }

  void resume_after_flush() noexcept {
    switch (state_->after_flush) {
      case ssl_resume_action::transport_read:
        state_->phase = ssl_io_phase::transport_read;
        submit_transport_read();
        return;
      case ssl_resume_action::finish:
        state_->phase = ssl_io_phase::done;
        state_->done = true;
        std::atomic_thread_fence(std::memory_order_release);
        complete_value(std::error_code{}, state_->bytes);
        return;
      case ssl_resume_action::application_read:
      case ssl_resume_action::application_write:
        state_->phase = ssl_io_phase::application;
        complete_value(std::error_code{}, 0);
        return;
      default:
        return;
    }
  }

  void submit_transport_read() noexcept {
    char* data = nullptr;
    const int available = BIO_nwrite0(read_bio(*state_->stream), &data);
    if (available <= 0) {
      state_->done = true;
      complete_value(last_ssl_error(), state_->bytes);
      return;
    }

    state_->transport_data = data;
    state_->transport_size = static_cast<std::size_t>(available);
    child_operation_.template emplace_from<read_operation_type>([this] {
      return bexec::connect(ssl_make_transport_read_sender(
                                state_->scheduler, *state_->stream,
                                state_->transport_data, state_->transport_size),
                            child_receiver(*this));
    });
    child_operation_.start();
  }

  void submit_transport_write() noexcept {
    child_operation_.template emplace_from<write_operation_type>([this] {
      return bexec::connect(ssl_make_transport_write_sender(
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
      // EOF（对端关闭）：transport 返回 0 字节且 ec 为空。终结为
      // connection_reset 并设 done=true 让 repeat_until 退出，否则
      // 会无限循环（SSL_read 不断得到 WANT_READ，transport 不断返回 0）。
      state_->done = true;
      complete_value(std::make_error_code(std::errc::connection_reset),
                     state_->bytes);
      return;
    }
    switch (state_->phase) {
      case ssl_io_phase::transport_read:
        handle_transport_read_complete(bytes);
        return;
      case ssl_io_phase::transport_write:
        handle_transport_write_complete(bytes);
        return;
      case ssl_io_phase::application:
      case ssl_io_phase::flush_output:
      case ssl_io_phase::done:
        complete_error(std::make_error_code(std::errc::protocol_error));
        return;
    }
  }

  void handle_transport_read_complete(std::size_t result) noexcept {
    // result <= 0 不再可能：ec 已在 handle_transport_complete 过滤

    char* data = nullptr;
    const int committed = BIO_nwrite(read_bio(*state_->stream), &data,
                                     ssl_bounded_int_size(result));
    if (committed != static_cast<int>(result)) {
      complete_error(
          last_ssl_error());  // 不变量违规：经 set_value(ec, bytes) 透传
      return;
    }

    state_->phase = ssl_io_phase::application;
    complete_value(std::error_code{}, 0);
  }

  void handle_transport_write_complete(std::size_t result) noexcept {
    // result <= 0 不再可能：ec 已在 handle_transport_complete 过滤

    char* data = nullptr;
    const int consumed = BIO_nread(write_bio(*state_->stream), &data,
                                   ssl_bounded_int_size(result));
    if (consumed != static_cast<int>(result)) {
      complete_error(
          last_ssl_error());  // 不变量违规：经 set_value(ec, bytes) 透传
      return;
    }

    state_->phase = ssl_io_phase::flush_output;
    complete_value(std::error_code{}, 0);
  }

  void complete_value(std::error_code ec, std::size_t bytes) noexcept {
    bexec::set_value(std::move(receiver_), ec, bytes);
  }

  void complete_error(std::error_code error) noexcept {
    // 不可恢复内部异常：通过 set_value(ec, bytes) 透传，让 repeat_receiver 走
    // complete_value 路径。 设 done=true 确保 repeat_until
    // 退出，否则不变量违规路径会无限循环。
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

}  // namespace bnio

#endif  // BNIO_DETAIL_SSL_ASYNC_OPERATIONS_READ_WRITE_STEP_H_
