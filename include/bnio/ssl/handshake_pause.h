/**
 * @file handshake_pause.h
 * @brief Public classification of SSL handshake pauses.
 */

#pragma once
#ifndef BNIO_SSL_HANDSHAKE_PAUSE_H_
#define BNIO_SSL_HANDSHAKE_PAUSE_H_

#include <bnio/export.h>

#include <system_error>

namespace bnio::ssl {

/**
 * Why an SSL handshake attempt paused before completing.
 *
 * The numeric values are stable public contract and unrelated to OpenSSL's
 * SSL_ERROR_* numbers; the mapping lives in
 * bnio/ssl/detail/handshake_pause_table.h.
 */
enum class handshake_pause_reason : int {
  /** Not a pause; also used for unknown pause-category values. */
  none = 0,

  /** OpenSSL needs readable transport data to make progress. */
  want_read = 1,

  /** OpenSSL needs writable transport data to make progress. */
  want_write = 2,

  /** OpenSSL paused for an X509 lookup callback. */
  x509_lookup = 3,

  /** OpenSSL paused for an asynchronous engine operation. */
  async = 4,

  /** OpenSSL paused waiting for an asynchronous job to be assigned. */
  async_job = 5,

  /** OpenSSL paused inside the client hello callback. */
  client_hello_cb = 6,

  /** OpenSSL paused for certificate verification retry. */
  retry_verify = 7,
};

/**
 * Returns the error category used to report SSL handshake pauses.
 *
 * Named "bnio.ssl.handshake_pause"; defined out of line so all translation
 * units and shared objects share one category object.
 */
[[nodiscard]] BNIO_EXPORT const std::error_category&
handshake_pause_category() noexcept;

/**
 * Creates an error_code identifying a handshake pause.
 *
 * handshake_pause_reason::none yields an empty code; every other value yields
 * a non-empty code in handshake_pause_category(). Use
 * handshake_pause_reason_of() instead of casting the stored value.
 */
[[nodiscard]] BNIO_EXPORT std::error_code make_handshake_pause_error(
    handshake_pause_reason reason) noexcept;

/**
 * Returns whether ec is a nonzero code in handshake_pause_category().
 */
[[nodiscard]] inline bool is_handshake_pause(
    const std::error_code& ec) noexcept {
  return ec.category() == handshake_pause_category() && ec.value() != 0;
}

/**
 * Returns the pause reason carried by ec, or none when ec is not a pause or
 * its value is not a known reason.
 */
[[nodiscard]] inline handshake_pause_reason handshake_pause_reason_of(
    const std::error_code& ec) noexcept {
  if (!is_handshake_pause(ec)) {
    return handshake_pause_reason::none;
  }
  const int value = ec.value();
  if (value < static_cast<int>(handshake_pause_reason::want_read) ||
      value > static_cast<int>(handshake_pause_reason::retry_verify)) {
    return handshake_pause_reason::none;
  }
  return static_cast<handshake_pause_reason>(value);
}

}  // namespace bnio::ssl

#endif  // BNIO_SSL_HANDSHAKE_PAUSE_H_
