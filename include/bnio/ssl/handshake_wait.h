/**
 * @file handshake_wait.h
 * @brief Public factory for the BIO-level wait step of a paused handshake.
 */

#pragma once
#ifndef BNIO_SSL_HANDSHAKE_WAIT_H_
#define BNIO_SSL_HANDSHAKE_WAIT_H_

#include <bnio/ssl/detail/handshake_wait.h>
#include <bnio/ssl/handshake_pause.h>
#include <bnio/ssl/tcp/stream_class.h>

#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::ssl {

/**
 * Creates a sender that performs the transport work requested by a handshake
 * pause.
 *
 * A pause means the attempt has not consumed or produced all of its transport
 * bytes yet. Each pause becomes exactly one transport step:
 *
 * - want_read: wait for encrypted bytes on `stream.lowest_layer()` and commit
 *   them into the stream's read BIO.
 * - want_write: drain encrypted output already queued in the write BIO to
 *   `stream.lowest_layer()`, repeating until the BIO is empty.
 * - other reasons (X509 lookup, async engine/job, client hello callback,
 *   certificate retry): complete with std::errc::operation_not_supported;
 *   these pauses are callback-driven, so the user factory must supply its own
 *   asynchronous work.
 *
 * A code that is not a handshake pause (see is_handshake_pause()) completes
 * with std::errc::invalid_argument. An invalid stream takes precedence and
 * completes with the dedicated no-OpenSSL-error value. `stream` must be the
 * one whose handshake produced `pause` and must outlive the returned
 * operation.
 *
 * The sender completes through set_value(std::error_code) and set_stopped()
 * only; a non-empty value reports a transport or classification failure. The
 * downstream stop token is forwarded to the transport operation. Completion
 * allocates no heap: the child operation is stored inline.
 */
template <class Scheduler, class NextLayer>
[[nodiscard]] auto make_handshake_wait_sender(Scheduler scheduler,
                                              tcp::stream<NextLayer>& stream,
                                              std::error_code pause) {
  return detail::handshake_wait_sender<std::remove_cvref_t<Scheduler>,
                                       NextLayer>(std::move(scheduler), stream,
                                                  pause);
}

}  // namespace bnio::ssl

#endif  // BNIO_SSL_HANDSHAKE_WAIT_H_
