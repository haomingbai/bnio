/**
 * @file shutdown.h
 * @brief SSL shutdown async operation.
 */

#pragma once
#ifndef BNIO_SSL_DETAIL_SHUTDOWN_H_
#define BNIO_SSL_DETAIL_SHUTDOWN_H_

#include <bnio/ssl/base/errors.h>
#include <bnio/ssl/detail/state_machine.h>

#include <bexec/receiver.hpp>
#include <utility>

namespace bnio::ssl {

/** @cond BNIO_DETAIL */
namespace detail {

template <class Scheduler, class NextLayer, class Receiver>
class shutdown_operation
    : public operation_base<shutdown_operation<Scheduler, NextLayer, Receiver>,
                            Scheduler, NextLayer, Receiver> {
 public:
  using base =
      operation_base<shutdown_operation<Scheduler, NextLayer, Receiver>,
                     Scheduler, NextLayer, Receiver>;

  shutdown_operation(Scheduler scheduler, tcp::stream<NextLayer>& stream,
                     Receiver receiver)
      : base(std::move(scheduler), stream, std::move(receiver)) {}

  void on_start() noexcept { run_shutdown(); }

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
    run_shutdown();
  }

  void deliver_value(std::error_code ec) noexcept {
    bexec::set_value(std::move(this->receiver_), ec);
  }

 private:
  void run_shutdown() noexcept {
    bnio::ssl::base::clear_errors();
    const int result = SSL_shutdown(this->stream_->native_handle());
    if (result == 1) {
      this->flush_then(resume_action::finish);
      return;
    }
    if (result == 0) {
      this->flush_then(resume_action::shutdown);
      return;
    }
    this->handle_ssl_error(result, resume_action::shutdown);
  }
};

}  // namespace detail
/** @endcond */

}  // namespace bnio::ssl

#endif  // BNIO_SSL_DETAIL_SHUTDOWN_H_
