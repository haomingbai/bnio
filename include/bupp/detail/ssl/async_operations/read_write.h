#pragma once
#ifndef BUPP_DETAIL_SSL_ASYNC_OPERATIONS_READ_WRITE_H_
#define BUPP_DETAIL_SSL_ASYNC_OPERATIONS_READ_WRITE_H_

#include <bupp/detail/ssl/async_operations/common.h>

#include <atomic>
#include <bexec/completion_signatures.hpp>
#include <bexec/detail/manual_lifetime.hpp>
#include <bexec/detail/operation_storage.hpp>
#include <bexec/receiver.hpp>
#include <bexec/repeat_until.hpp>
#include <bexec/sender.hpp>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bupp {

/** @cond BUPP_DETAIL */
namespace detail {

enum class ssl_application_io {
  read,
  write,
};

enum class ssl_io_phase {
  application,
  flush_output,
  transport_read,
  transport_write,
  done,
};

template <class Scheduler, class NextLayer, class Holder, bool DirectSubmit,
          class Receiver, ssl_application_io Application>
class ssl_io_operation;

template <class State, bool DirectSubmit, class Receiver>
class ssl_io_step_operation;

template <class Scheduler, class NextLayer, class Holder,
          ssl_application_io Application>
struct ssl_io_state {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using next_layer_type = NextLayer;
  static constexpr ssl_application_io application = Application;
  static constexpr ssl_resume_action application_action =
      Application == ssl_application_io::read
          ? ssl_resume_action::application_read
          : ssl_resume_action::application_write;

  ssl_io_state(scheduler_type scheduler, ssl_stream<NextLayer>& stream,
               Holder buffer)
      : scheduler(std::move(scheduler)),
        stream(&stream),
        buffer(std::move(buffer)) {}

  scheduler_type scheduler;
  ssl_stream<NextLayer>* stream;
  Holder buffer;
  char* transport_data = nullptr;
  std::size_t transport_size = 0;
  std::size_t bytes = 0;
  ssl_io_phase phase = ssl_io_phase::application;
  ssl_resume_action after_flush = application_action;
  bool done = false;
};

template <class State, bool DirectSubmit>
class ssl_io_step_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  explicit ssl_io_step_sender(State* state) noexcept : state_(state) {}

  template <class Receiver>
  auto connect(Receiver receiver) const {
    return ssl_io_step_operation<State, DirectSubmit,
                                 std::remove_cvref_t<Receiver>>(
        state_, std::move(receiver));
  }

 private:
  State* state_;
};

template <class State, bool DirectSubmit>
class ssl_io_step_factory {
 public:
  explicit ssl_io_step_factory(State* state) noexcept : state_(state) {}

  [[nodiscard]] auto operator()() const noexcept {
    return ssl_io_step_sender<State, DirectSubmit>(state_);
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

template <class State, bool DirectSubmit, class Receiver>
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

    void set_value(std::size_t bytes) noexcept {
      operation_->handle_transport_complete(bytes);
    }

    void set_error(std::error_code error) noexcept {
      operation_->complete_error(error);
    }

    void set_stopped() noexcept { operation_->complete_stopped(); }

   private:
    ssl_io_step_operation* operation_;
  };

  using read_sender_type =
      decltype(ssl_make_transport_read_sender<DirectSubmit>(
          std::declval<typename State::scheduler_type&>(),
          std::declval<ssl_stream<typename State::next_layer_type>&>(),
          static_cast<void*>(nullptr), std::size_t{}));
  using write_sender_type =
      decltype(ssl_make_transport_write_sender<DirectSubmit>(
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
      complete_stopped();
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
        complete_value(state_->bytes);
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
      const int result =
          SSL_write(state_->stream->native_handle(), state_->buffer.data(),
                    ssl_bounded_int_size(state_->buffer.size()));
      handle_application_result(result);
    }
  }

  void handle_application_result(int result) noexcept {
    if (result > 0) {
      state_->bytes = static_cast<std::size_t>(result);
      state_->after_flush = ssl_resume_action::finish;
      state_->phase = ssl_io_phase::flush_output;
      complete_value(0);
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
        complete_value(0);
        return;
      case SSL_ERROR_WANT_WRITE:
        state_->after_flush = State::application_action;
        state_->phase = ssl_io_phase::flush_output;
        complete_value(0);
        return;
      case SSL_ERROR_ZERO_RETURN:
        complete_error(std::make_error_code(std::errc::connection_reset));
        return;
      default:
        complete_error(last_ssl_error());
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
      complete_error(last_ssl_error());
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
        complete_value(state_->bytes);
        return;
      case ssl_resume_action::application_read:
      case ssl_resume_action::application_write:
        state_->phase = ssl_io_phase::application;
        complete_value(0);
        return;
      case ssl_resume_action::handshake:
      case ssl_resume_action::shutdown:
        complete_error(std::make_error_code(std::errc::protocol_error));
        return;
    }
  }

  void submit_transport_read() noexcept {
    char* data = nullptr;
    const int available = BIO_nwrite0(read_bio(*state_->stream), &data);
    if (available <= 0) {
      complete_error(last_ssl_error());
      return;
    }

    state_->transport_data = data;
    state_->transport_size = static_cast<std::size_t>(available);
    child_operation_.template emplace_from<read_operation_type>([this] {
      return bexec::connect(ssl_make_transport_read_sender<DirectSubmit>(
                                state_->scheduler, *state_->stream,
                                state_->transport_data, state_->transport_size),
                            child_receiver(*this));
    });
    child_operation_.start();
  }

  void submit_transport_write() noexcept {
    child_operation_.template emplace_from<write_operation_type>([this] {
      return bexec::connect(ssl_make_transport_write_sender<DirectSubmit>(
                                state_->scheduler, *state_->stream,
                                state_->transport_data, state_->transport_size),
                            child_receiver(*this));
    });
    child_operation_.start();
  }

  void handle_transport_complete(std::size_t bytes) noexcept {
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
    if (result <= 0) {
      complete_error(std::make_error_code(std::errc::connection_reset));
      return;
    }

    char* data = nullptr;
    const int committed = BIO_nwrite(read_bio(*state_->stream), &data,
                                     ssl_bounded_int_size(result));
    if (committed != static_cast<int>(result)) {
      complete_error(last_ssl_error());
      return;
    }

    state_->phase = ssl_io_phase::application;
    complete_value(0);
  }

  void handle_transport_write_complete(std::size_t result) noexcept {
    if (result <= 0) {
      complete_error(std::make_error_code(std::errc::connection_reset));
      return;
    }

    char* data = nullptr;
    const int consumed = BIO_nread(write_bio(*state_->stream), &data,
                                   ssl_bounded_int_size(result));
    if (consumed != static_cast<int>(result)) {
      complete_error(last_ssl_error());
      return;
    }

    state_->phase = ssl_io_phase::flush_output;
    complete_value(0);
  }

  void complete_value(std::size_t bytes) noexcept {
    bexec::set_value(std::move(receiver_), bytes);
  }

  void complete_error(std::error_code error) noexcept {
    bexec::set_error(std::move(receiver_), error);
  }

  void complete_stopped() noexcept { bexec::set_stopped(std::move(receiver_)); }

  State* state_;
  receiver_type receiver_;
  child_operations_type child_operation_;
};

template <class Scheduler, class NextLayer, class Holder, bool DirectSubmit,
          class Receiver, ssl_application_io Application>
class ssl_io_operation {
 public:
  using state_type = ssl_io_state<Scheduler, NextLayer, Holder, Application>;
  using receiver_type = std::remove_cvref_t<Receiver>;
  using factory_type = ssl_io_step_factory<state_type, DirectSubmit>;
  using predicate_type = ssl_io_done_predicate<state_type>;
  using repeat_sender_type = decltype(bexec::repeat_until(
      std::declval<factory_type>(), std::declval<predicate_type>()));

  class repeat_receiver {
   public:
    explicit repeat_receiver(ssl_io_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] decltype(auto) get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::size_t bytes) noexcept {
      operation_->complete_value(bytes);
    }

    template <class Error>
    void set_error(Error&& error) noexcept {
      operation_->complete_error(std::forward<Error>(error));
    }

    void set_stopped() noexcept { operation_->complete_stopped(); }

   private:
    ssl_io_operation* operation_;
  };

  using repeat_operation_type = decltype(bexec::connect(
      std::declval<repeat_sender_type>(), std::declval<repeat_receiver>()));

  ssl_io_operation(std::remove_cvref_t<Scheduler> scheduler,
                   ssl_stream<NextLayer>& stream, Holder buffer,
                   Receiver receiver)
      : state_(std::move(scheduler), stream, std::move(buffer)),
        receiver_(std::move(receiver)) {
    repeat_operation_.emplace_from([this] {
      return bexec::connect(
          bexec::repeat_until(factory_type(&state_), predicate_type(&state_)),
          repeat_receiver(*this));
    });
  }

  ssl_io_operation(const ssl_io_operation&) = delete;
  ssl_io_operation& operator=(const ssl_io_operation&) = delete;
  ssl_io_operation(ssl_io_operation&&) = delete;
  ssl_io_operation& operator=(ssl_io_operation&&) = delete;

  void start() noexcept { bexec::start(*repeat_operation_); }

 private:
  void complete_value(std::size_t bytes) noexcept {
    if constexpr (Application == ssl_application_io::read) {
      state_.buffer.commit(bytes);
    }
    bexec::set_value(std::move(receiver_), bytes);
  }

  void complete_error(std::error_code error) noexcept {
    bexec::set_error(std::move(receiver_), error);
  }

  template <class Error>
  void complete_error(Error&&) noexcept {
    bexec::set_error(std::move(receiver_),
                     std::make_error_code(std::errc::protocol_error));
  }

  void complete_stopped() noexcept { bexec::set_stopped(std::move(receiver_)); }

  state_type state_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<repeat_operation_type> repeat_operation_;
};

template <class Scheduler, class NextLayer, class Holder, bool DirectSubmit,
          class Receiver>
using ssl_read_operation =
    ssl_io_operation<Scheduler, NextLayer, Holder, DirectSubmit, Receiver,
                     ssl_application_io::read>;

template <class Scheduler, class NextLayer, class Holder, bool DirectSubmit,
          class Receiver>
using ssl_write_operation =
    ssl_io_operation<Scheduler, NextLayer, Holder, DirectSubmit, Receiver,
                     ssl_application_io::write>;

}  // namespace detail
/** @endcond */

}  // namespace bupp

#endif  // BUPP_DETAIL_SSL_ASYNC_OPERATIONS_READ_WRITE_H_
