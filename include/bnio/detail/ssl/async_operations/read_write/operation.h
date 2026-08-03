/**
 * @file operation.h
 * @brief SSL read/write operation state.
 */

#pragma once
#ifndef BNIO_DETAIL_SSL_ASYNC_OPERATIONS_READ_WRITE_OPERATION_H_
#define BNIO_DETAIL_SSL_ASYNC_OPERATIONS_READ_WRITE_OPERATION_H_

#include <bnio/detail/ssl/async_operations/read_write/step.h>

#include <bexec/detail/manual_lifetime.hpp>
#include <bexec/receiver.hpp>
#include <bexec/repeat_until.hpp>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio {

/** @cond BNIO_DETAIL */
namespace detail {

template <class Scheduler, class NextLayer, class Holder, class Receiver,
          ssl_application_io Application, bool CompleteBuffer>
class ssl_io_operation {
 public:
  using state_type =
      ssl_io_state<Scheduler, NextLayer, Holder, Application, CompleteBuffer>;
  using receiver_type = std::remove_cvref_t<Receiver>;
  using factory_type = ssl_io_step_factory<state_type>;
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

    void set_value(std::error_code ec, std::size_t bytes) noexcept {
      operation_->complete_value(ec, bytes);
    }

    template <class Error>
    void set_error(Error&& error) noexcept {
      // bexec::repeat_until's completion_signatures unconditionally include
      // set_error_t(std::exception_ptr); this member is required for
      // compilation and is never reached at runtime (child steps only send
      // set_value/set_stopped, and neither predicate nor factory throws).
      // Defensively forward to complete_error (via set_value(ec, bytes)).
      operation_->complete_error(std::forward<Error>(error));
    }

    void set_stopped() noexcept {
      // Distinguish stop-token cancellation from io_context::stop().
      // repeat_until::drain() checks the stop_token before each round and
      // sends set_stopped if true — that is stop-token cancellation reaching
      // step::child_receiver::set_stopped(). An io_context::stop() interruption
      // from the transport layer is the case where set_stopped should be
      // preserved.
      if (ssl_stop_requested(operation_->receiver_)) {
        operation_->complete_canceled();
      } else {
        operation_->complete_stopped();
      }
    }

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

  void start() noexcept {
    if (ssl_stop_requested(receiver_)) {
      complete_value(std::make_error_code(std::errc::operation_canceled), 0);
      return;
    }
    if (empty_buffer()) {
      complete_value(std::error_code{}, 0);
      return;
    }

    bexec::start(*repeat_operation_);
  }

 private:
  [[nodiscard]] bool empty_buffer() const noexcept {
    if constexpr (Application == ssl_application_io::read) {
      return state_.buffer.view().size == 0;
    } else {
      return state_.buffer.size() == 0;
    }
  }

  void complete_value(std::error_code ec, std::size_t bytes) noexcept {
    if constexpr (Application == ssl_application_io::read) {
      if (!ec) {
        state_.buffer.commit(bytes);
      }
      bexec::set_value(std::move(receiver_), ec, bytes);
    } else {
      bexec::set_value(std::move(receiver_), ec, state_.bytes);
    }
  }

  void complete_canceled() noexcept {
    // stop-token cancellation: report ec=operation_canceled with the bytes
    // transferred so far
    bexec::set_value(std::move(receiver_),
                     std::make_error_code(std::errc::operation_canceled),
                     state_.bytes);
  }

  void complete_error(std::error_code error) noexcept {
    // Unrecoverable internal exception: pass through to the receiver via
    // set_value(ec, bytes)
    bexec::set_value(std::move(receiver_), error, state_.bytes);
  }

  template <class Error>
  void complete_error(Error&&) noexcept {
    // Unrecoverable internal exception (unknown error type)
    bexec::set_value(std::move(receiver_),
                     std::make_error_code(std::errc::protocol_error),
                     state_.bytes);
  }

  void complete_stopped() noexcept { bexec::set_stopped(std::move(receiver_)); }

  state_type state_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<repeat_operation_type> repeat_operation_;
};

template <class Scheduler, class NextLayer, class Holder, class Receiver>
using ssl_read_operation =
    ssl_io_operation<Scheduler, NextLayer, Holder, Receiver,
                     ssl_application_io::read, false>;

template <class Scheduler, class NextLayer, class Holder, class Receiver>
using ssl_write_operation =
    ssl_io_operation<Scheduler, NextLayer, Holder, Receiver,
                     ssl_application_io::write, true>;

template <class Scheduler, class NextLayer, class Holder, class Receiver>
using ssl_write_some_operation =
    ssl_io_operation<Scheduler, NextLayer, Holder, Receiver,
                     ssl_application_io::write, false>;

}  // namespace detail
/** @endcond */

}  // namespace bnio

#endif  // BNIO_DETAIL_SSL_ASYNC_OPERATIONS_READ_WRITE_OPERATION_H_
