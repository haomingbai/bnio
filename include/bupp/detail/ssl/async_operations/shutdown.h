#pragma once
#ifndef BUPP_DETAIL_SSL_ASYNC_OPERATIONS_SHUTDOWN_H_
#define BUPP_DETAIL_SSL_ASYNC_OPERATIONS_SHUTDOWN_H_

#include <bupp/detail/ssl/async_operations/state_machine.h>

#include <bexec/receiver.hpp>
#include <utility>

namespace bupp {

/** @cond BUPP_DETAIL */
namespace detail {

template <class Scheduler, class NextLayer, bool DirectSubmit, class Receiver>
class ssl_shutdown_operation
    : public ssl_async_operation_base<
          ssl_shutdown_operation<Scheduler, NextLayer, DirectSubmit, Receiver>,
          Scheduler, NextLayer, Receiver, DirectSubmit> {
 public:
  using base = ssl_async_operation_base<
      ssl_shutdown_operation<Scheduler, NextLayer, DirectSubmit, Receiver>,
      Scheduler, NextLayer, Receiver, DirectSubmit>;

  ssl_shutdown_operation(Scheduler scheduler, ssl_stream<NextLayer>& stream,
                         Receiver receiver)
      : base(std::move(scheduler), stream, std::move(receiver)) {}

  void on_start() noexcept { run_shutdown(); }

  void resume(ssl_resume_action action) noexcept {
    if (action == ssl_resume_action::finish) {
      this->post_complete_value();
      return;
    }
    run_shutdown();
  }

  void deliver_value() noexcept {
    bexec::set_value(std::move(this->receiver_));
  }

 private:
  void run_shutdown() noexcept {
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

}  // namespace bupp

#endif  // BUPP_DETAIL_SSL_ASYNC_OPERATIONS_SHUTDOWN_H_
