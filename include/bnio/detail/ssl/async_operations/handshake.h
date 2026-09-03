/**
 * @file handshake.h
 * @brief SSL handshake async operation.
 */

#pragma once
#ifndef BNIO_DETAIL_SSL_ASYNC_OPERATIONS_HANDSHAKE_H_
#define BNIO_DETAIL_SSL_ASYNC_OPERATIONS_HANDSHAKE_H_

#include <bnio/detail/ssl/async_operations/state_machine.h>

#include <bexec/receiver.hpp>
#include <utility>

namespace bnio {

/** @cond BNIO_DETAIL */
namespace detail {

template <class Scheduler, class NextLayer, class Receiver>
class ssl_handshake_operation
    : public ssl_async_operation_base<
          ssl_handshake_operation<Scheduler, NextLayer, Receiver>, Scheduler,
          NextLayer, Receiver> {
 public:
  using base = ssl_async_operation_base<
      ssl_handshake_operation<Scheduler, NextLayer, Receiver>, Scheduler,
      NextLayer, Receiver>;

  ssl_handshake_operation(Scheduler scheduler, ssl_stream<NextLayer>& stream,
                          ssl_handshake_type type, Receiver receiver)
      : base(std::move(scheduler), stream, std::move(receiver)), type_(type) {}

  void on_start() noexcept {
    if (!this->stream_->valid()) {
      // No OpenSSL call happens on this path, so there is no OpenSSL error
      // to read: reporting the dedicated no-OpenSSL-error value is the only
      // honest attribution. Reading the thread-local queue here would
      // surface a stale entry from unrelated earlier OpenSSL work.
      this->post_complete_error(make_no_ssl_error());
      return;
    }

    if (type_ == ssl_handshake_type::client) {
      SSL_set_connect_state(this->stream_->native_handle());
    } else {
      SSL_set_accept_state(this->stream_->native_handle());
    }
    run_handshake();
  }

  void resume(ssl_resume_action action) noexcept {
    if (action == ssl_resume_action::finish) {
      this->post_complete_value(std::error_code{});
      return;
    }
    run_handshake();
  }

  void deliver_value(std::error_code ec) noexcept {
    bexec::set_value(std::move(this->receiver_), ec);
  }

 private:
  void run_handshake() noexcept {
    clear_ssl_errors();
    const int result = SSL_do_handshake(this->stream_->native_handle());
    if (result == 1) {
      this->flush_then(ssl_resume_action::finish);
      return;
    }
    this->handle_ssl_error(result, ssl_resume_action::handshake);
  }

  ssl_handshake_type type_;
};

}  // namespace detail
/** @endcond */

}  // namespace bnio

#endif  // BNIO_DETAIL_SSL_ASYNC_OPERATIONS_HANDSHAKE_H_
