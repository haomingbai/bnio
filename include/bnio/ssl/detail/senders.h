/**
 * @file senders.h
 * @brief SSL async operation sender types.
 */

#pragma once
#ifndef BNIO_SSL_DETAIL_SENDERS_H_
#define BNIO_SSL_DETAIL_SENDERS_H_

#include <bnio/ssl/context.h>
#include <bnio/ssl/detail/handshake_retry.h>
#include <bnio/ssl/detail/read_write.h>
#include <bnio/ssl/detail/shutdown.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/just.hpp>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::ssl {

/** @cond BNIO_DETAIL */
namespace detail {

/**
 * Factory used by the 2-argument async_handshake.
 *
 * The retry operation drives transport pauses itself, so this factory is
 * reached only for callback-driven pauses. The 2-argument overload has no
 * resume surface; completing with operation_not_supported terminates such a
 * handshake instead of spinning on a pause the caller cannot resume.
 */
struct handshake_default_factory {
  [[nodiscard]] auto operator()(const std::error_code&) const {
    return bexec::just(
        std::make_error_code(std::errc::operation_not_supported));
  }
};

/**
 * Sender for the 2-argument async_handshake.
 *
 * An instance of the factory-driven retry sender whose factory performs no
 * work: transport pauses are driven internally by the retry operation, and a
 * callback-driven pause completes with std::errc::operation_not_supported
 * because this overload has no resume surface.
 */
template <class Scheduler, class NextLayer>
using handshake_sender =
    handshake_retry_sender<Scheduler, NextLayer, handshake_default_factory>;

/** Sender for one plaintext SSL read operation. */
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

/** Sender for one plaintext SSL write operation. */
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
                        std::remove_cvref_t<Receiver>, application_io::write,
                        CompleteBuffer>(std::move(scheduler_), *stream_,
                                        std::move(buffer_),
                                        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  tcp::stream<NextLayer>* stream_;
  Holder buffer_;
};

/** Sender for the SSL shutdown operation. */
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
