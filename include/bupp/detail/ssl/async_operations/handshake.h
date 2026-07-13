#pragma once
#ifndef BUPP_DETAIL_SSL_ASYNC_OPERATIONS_HANDSHAKE_H_
#define BUPP_DETAIL_SSL_ASYNC_OPERATIONS_HANDSHAKE_H_

#include <bupp/detail/ssl/async_operations/state_machine.h>

#include <bexec/receiver.hpp>
#include <utility>

namespace bupp {

/** @cond BUPP_DETAIL */
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
      this->post_complete_error(last_ssl_error());
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
      this->post_complete_value();
      return;
    }
    run_handshake();
  }

  void deliver_value() noexcept {
    bexec::set_value(std::move(this->receiver_));
  }

 private:
  void run_handshake() noexcept {
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

}  // namespace bupp

#endif  // BUPP_DETAIL_SSL_ASYNC_OPERATIONS_HANDSHAKE_H_
