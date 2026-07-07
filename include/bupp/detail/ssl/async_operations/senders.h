#pragma once
#ifndef BUPP_DETAIL_SSL_ASYNC_OPERATIONS_SENDERS_H_
#define BUPP_DETAIL_SSL_ASYNC_OPERATIONS_SENDERS_H_

#include <bupp/detail/ssl/async_operations/handshake.h>
#include <bupp/detail/ssl/async_operations/read_write.h>
#include <bupp/detail/ssl/async_operations/shutdown.h>

#include <bexec/completion_signatures.hpp>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bupp {

/** @cond BUPP_DETAIL */
namespace detail {

template <class Scheduler, class NextLayer, bool DirectSubmit>
class ssl_handshake_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  ssl_handshake_sender(Scheduler scheduler, ssl_stream<NextLayer>& stream,
                       ssl_handshake_type type) noexcept
      : scheduler_(std::move(scheduler)), stream_(&stream), type_(type) {}

  template <class Receiver>
  auto connect(Receiver receiver) const {
    return ssl_handshake_operation<Scheduler, NextLayer, DirectSubmit,
                                   std::remove_cvref_t<Receiver>>(
        scheduler_, *stream_, type_, std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  ssl_stream<NextLayer>* stream_;
  ssl_handshake_type type_;
};

template <class Scheduler, class NextLayer, class Holder, bool DirectSubmit>
class ssl_read_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  ssl_read_sender(Scheduler scheduler, ssl_stream<NextLayer>& stream,
                  Holder buffer)
      : scheduler_(std::move(scheduler)),
        stream_(&stream),
        buffer_(std::move(buffer)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return ssl_read_operation<Scheduler, NextLayer, Holder, DirectSubmit,
                              std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), *stream_, std::move(buffer_),
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  ssl_stream<NextLayer>* stream_;
  Holder buffer_;
};

template <class Scheduler, class NextLayer, class Holder, bool DirectSubmit,
          bool CompleteBuffer>
class ssl_write_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  ssl_write_sender(Scheduler scheduler, ssl_stream<NextLayer>& stream,
                   Holder buffer)
      : scheduler_(std::move(scheduler)),
        stream_(&stream),
        buffer_(std::move(buffer)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return ssl_io_operation<Scheduler, NextLayer, Holder, DirectSubmit,
                            std::remove_cvref_t<Receiver>,
                            ssl_application_io::write, CompleteBuffer>(
        std::move(scheduler_), *stream_, std::move(buffer_),
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  ssl_stream<NextLayer>* stream_;
  Holder buffer_;
};

template <class Scheduler, class NextLayer, bool DirectSubmit>
class ssl_shutdown_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  ssl_shutdown_sender(Scheduler scheduler, ssl_stream<NextLayer>& stream)
      : scheduler_(std::move(scheduler)), stream_(&stream) {}

  template <class Receiver>
  auto connect(Receiver receiver) const {
    return ssl_shutdown_operation<Scheduler, NextLayer, DirectSubmit,
                                  std::remove_cvref_t<Receiver>>(
        scheduler_, *stream_, std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  ssl_stream<NextLayer>* stream_;
};

}  // namespace detail
/** @endcond */

}  // namespace bupp

#endif  // BUPP_DETAIL_SSL_ASYNC_OPERATIONS_SENDERS_H_
