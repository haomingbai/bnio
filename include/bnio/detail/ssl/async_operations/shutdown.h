/**
 * @file shutdown.h
 * @brief SSL shutdown async operation.
 */

#pragma once
#ifndef BNIO_DETAIL_SSL_ASYNC_OPERATIONS_SHUTDOWN_H_
#define BNIO_DETAIL_SSL_ASYNC_OPERATIONS_SHUTDOWN_H_

#include <bnio/detail/ssl/async_operations/state_machine.h>

#include <bexec/receiver.hpp>
#include <utility>

namespace bnio {

/** @cond BNIO_DETAIL */
namespace detail {

template <class Scheduler, class NextLayer, class Receiver>
class ssl_shutdown_operation
    : public ssl_async_operation_base<
          ssl_shutdown_operation<Scheduler, NextLayer, Receiver>, Scheduler,
          NextLayer, Receiver> {
 public:
  using base = ssl_async_operation_base<
      ssl_shutdown_operation<Scheduler, NextLayer, Receiver>, Scheduler,
      NextLayer, Receiver>;

  ssl_shutdown_operation(Scheduler scheduler, ssl_stream<NextLayer>& stream,
                         Receiver receiver)
      : base(std::move(scheduler), stream, std::move(receiver)) {}

  void on_start() noexcept { run_shutdown(); }

  void resume(ssl_resume_action action) noexcept {
    if (action == ssl_resume_action::finish) {
      this->post_complete_value(std::error_code{});
      return;
    }
    run_shutdown();
  }

  void deliver_value(std::error_code ec) noexcept {
    bexec::set_value(std::move(this->receiver_), ec);
  }

 private:
  void run_shutdown() noexcept {
    clear_ssl_errors();
    const int result = SSL_shutdown(this->stream_->native_handle());
    if (result == 1) {
      this->flush_then(ssl_resume_action::finish);
      return;
    }
    if (result == 0) {
      this->flush_then(ssl_resume_action::shutdown);
      return;
    }
    this->handle_ssl_error(result, ssl_resume_action::shutdown);
  }
};

}  // namespace detail
/** @endcond */

}  // namespace bnio

#endif  // BNIO_DETAIL_SSL_ASYNC_OPERATIONS_SHUTDOWN_H_
