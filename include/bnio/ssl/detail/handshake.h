/**
 * @file handshake.h
 * @brief SSL handshake async operation.
 */

#pragma once
#ifndef BNIO_SSL_DETAIL_HANDSHAKE_H_
#define BNIO_SSL_DETAIL_HANDSHAKE_H_

#include <bnio/ssl/base/errors.h>
#include <bnio/ssl/context.h>
#include <bnio/ssl/detail/state_machine.h>

#include <bexec/receiver.hpp>
#include <utility>

namespace bnio::ssl {

/** @cond BNIO_DETAIL */
namespace detail {

template <class Scheduler, class NextLayer, class Receiver>
class handshake_operation
    : public operation_base<handshake_operation<Scheduler, NextLayer, Receiver>,
                            Scheduler, NextLayer, Receiver> {
 public:
  using base =
      operation_base<handshake_operation<Scheduler, NextLayer, Receiver>,
                     Scheduler, NextLayer, Receiver>;

  handshake_operation(Scheduler scheduler, tcp::stream<NextLayer>& stream,
                      handshake_type type, Receiver receiver)
      : base(std::move(scheduler), stream, std::move(receiver)), type_(type) {}

  void on_start() noexcept {
    if (!this->stream_->valid()) {
      // No OpenSSL call happens on this path, so there is no OpenSSL error
      // to read: reporting the dedicated no-OpenSSL-error value is the only
      // honest attribution. Reading the thread-local queue here would
      // surface a stale entry from unrelated earlier OpenSSL work.
      this->post_complete_error(bnio::ssl::base::make_no_ssl_error());
      return;
    }

    if (type_ == handshake_type::client) {
      SSL_set_connect_state(this->stream_->native_handle());
    } else {
      SSL_set_accept_state(this->stream_->native_handle());
    }
    run_handshake();
  }

  void resume(resume_action action) noexcept {
    if (action == resume_action::finish) {
      this->post_complete_value(bnio::ssl::base::empty_error_code);
      return;
    }
    if (action == resume_action::fail) {
      // The pending output (e.g. the fatal alert) has been flushed; deliver
      // the staged SSL error.
      this->post_complete_error(this->pending_error_);
      return;
    }
    run_handshake();
  }

  void deliver_value(std::error_code ec) noexcept {
    bexec::set_value(std::move(this->receiver_), ec);
  }

 private:
  void run_handshake() noexcept {
    bnio::ssl::base::clear_errors();
    const int result = SSL_do_handshake(this->stream_->native_handle());
    if (result == 1) {
      this->flush_then(resume_action::finish);
      return;
    }
    this->handle_ssl_error(result, resume_action::handshake);
  }

  handshake_type type_;
};

}  // namespace detail
/** @endcond */

}  // namespace bnio::ssl

#endif  // BNIO_SSL_DETAIL_HANDSHAKE_H_
