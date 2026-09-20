/**
 * @file senders.h
 * @brief SSL async operation sender types.
 */

#pragma once
#ifndef BNIO_SSL_DETAIL_SENDERS_H_
#define BNIO_SSL_DETAIL_SENDERS_H_

#include <bnio/ssl/context.h>
#include <bnio/ssl/detail/handshake.h>
#include <bnio/ssl/detail/read_write.h>
#include <bnio/ssl/detail/shutdown.h>

#include <bexec/completion_signatures.hpp>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::ssl {

/** @cond BNIO_DETAIL */
namespace detail {

template <class Scheduler, class NextLayer>
class handshake_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                   bexec::set_stopped_t()>;

  handshake_sender(Scheduler scheduler, tcp::stream<NextLayer>& stream,
                   handshake_type type) noexcept
      : scheduler_(std::move(scheduler)), stream_(&stream), type_(type) {}

  template <class Receiver>
  auto connect(Receiver receiver) const {
    return handshake_operation<Scheduler, NextLayer,
                               std::remove_cvref_t<Receiver>>(
        scheduler_, *stream_, type_, std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  tcp::stream<NextLayer>* stream_;
  handshake_type type_;
};

template <class Scheduler, class NextLayer, class Holder>
class read_sender {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  read_sender(Scheduler scheduler, tcp::stream<NextLayer>& stream,
              Holder buffer)
      : scheduler_(std::move(scheduler)),
        stream_(&stream),
        buffer_(std::move(buffer)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return read_operation<Scheduler, NextLayer, Holder,
                          std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), *stream_, std::move(buffer_),
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  tcp::stream<NextLayer>* stream_;
  Holder buffer_;
};

template <class Scheduler, class NextLayer, class Holder, bool CompleteBuffer>
class write_sender {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  write_sender(Scheduler scheduler, tcp::stream<NextLayer>& stream,
               Holder buffer)
      : scheduler_(std::move(scheduler)),
        stream_(&stream),
        buffer_(std::move(buffer)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return io_operation<Scheduler, NextLayer, Holder,
                        std::remove_cvref_t<Receiver>,
                        application_io::write, CompleteBuffer>(
        std::move(scheduler_), *stream_, std::move(buffer_),
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  tcp::stream<NextLayer>* stream_;
  Holder buffer_;
};

template <class Scheduler, class NextLayer>
class shutdown_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                   bexec::set_stopped_t()>;

  shutdown_sender(Scheduler scheduler, tcp::stream<NextLayer>& stream)
      : scheduler_(std::move(scheduler)), stream_(&stream) {}

  template <class Receiver>
  auto connect(Receiver receiver) const {
    return shutdown_operation<Scheduler, NextLayer,
                              std::remove_cvref_t<Receiver>>(
        scheduler_, *stream_, std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  tcp::stream<NextLayer>* stream_;
};

}  // namespace detail
/** @endcond */

}  // namespace bnio::ssl

#endif  // BNIO_SSL_DETAIL_SENDERS_H_
