/**
 * @file errors.h
 * @brief OpenSSL error category and error_code helpers.
 */

#pragma once
#ifndef BNIO_SSL_BASE_ERRORS_H_
#define BNIO_SSL_BASE_ERRORS_H_

#include <bnio/export.h>
#include <openssl/err.h>

#include <system_error>

namespace bnio::ssl::base {

/**
 * Returns the error category used for OpenSSL library errors.
 */
[[nodiscard]] BNIO_EXPORT const std::error_category&
openssl_error_category() noexcept;

/**
 * Creates an error_code in the OpenSSL error category.
 */
[[nodiscard]] BNIO_EXPORT std::error_code make_openssl_error(
    unsigned long code) noexcept;

/**
 * Returns the error_code bnio reports when an SSL operation's failure path
 * recorded no OpenSSL error at all (for example, a handshake on an invalid
 * stream never reaches OpenSSL). The value lives in the OpenSSL error
 * category, never collides with a real OpenSSL error code, and does not
 * represent any TLS-level failure.
 */
[[nodiscard]] BNIO_EXPORT std::error_code make_no_ssl_error() noexcept;

/**
 * Clears the calling thread's thread-local OpenSSL error queue.
 *
 * Every OpenSSL call whose failure is reported through the error queue is
 * immediately preceded by this call, so the error read after a failure
 * belongs to that call and is never a leftover from earlier OpenSSL work on
 * the same thread. The queue is thread-local and only reflects the calling
 * thread's own OpenSSL activity, so clearing it races with nothing.
 */
inline void clear_errors() noexcept { ERR_clear_error(); }

/**
 * Drains the thread-local OpenSSL error queue after a failing OpenSSL call,
 * returning the first recorded error as an error_code in the OpenSSL error
 * category.
 *
 * The queue must have been cleared right before the failing call (see
 * clear_errors), so the first popped error belongs to that call. One failed
 * call can enqueue several error codes; the rest are cleared so nothing
 * leaks into a later queue reader. An empty queue (the failure path recorded
 * no OpenSSL error at all, for example when the call never reached OpenSSL)
 * yields the dedicated no-OpenSSL-error value instead of impersonating a
 * real TLS-level failure.
 */
[[nodiscard]] inline std::error_code last_error() noexcept {
  const unsigned long error = ERR_get_error();
  ERR_clear_error();
  if (error == 0) {
    return make_no_ssl_error();
  }
  return make_openssl_error(error);
}

/**
 * Shared empty error code for success completion paths, instead of
 * default-constructing std::error_code{}, whose default constructor
 * consults system_category() on every call. This value uses the same
 * category as the default constructor, so `ec == std::error_code{}`
 * semantics are preserved.
 */
inline const std::error_code empty_error_code{0, std::system_category()};

}  // namespace bnio::ssl::base

#endif  // BNIO_SSL_BASE_ERRORS_H_
