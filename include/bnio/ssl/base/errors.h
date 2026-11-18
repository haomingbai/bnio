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

/** Returns the error category used for OpenSSL library errors. */
[[nodiscard]] BNIO_EXPORT const std::error_category&
openssl_error_category() noexcept;

/** Creates an error_code in the OpenSSL error category. */
[[nodiscard]] BNIO_EXPORT std::error_code make_openssl_error(
    unsigned long code) noexcept;

/**
 * Returns the error_code reported when an SSL failure path recorded no
 * OpenSSL error (e.g. an invalid stream never reaches OpenSSL). It lives in
 * the OpenSSL category but is not a TLS-level failure and never collides with
 * a real OpenSSL error code.
 */
[[nodiscard]] BNIO_EXPORT std::error_code make_no_ssl_error() noexcept;

/**
 * Clears the calling thread's thread-local OpenSSL error queue. Call it right
 * before an OpenSSL call whose failure will be read with last_error(); the
 * queue is thread-local, so this races with nothing.
 */
inline void clear_errors() noexcept { ERR_clear_error(); }

/**
 * Returns the first OpenSSL error as an error_code after a failing call and
 * clears the rest. The queue must have been cleared immediately before that
 * call (see clear_errors). An empty queue yields make_no_ssl_error() rather
 * than impersonating a real TLS-level failure.
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
 * Shared empty error_code for success paths, avoiding the system_category()
 * lookup in std::error_code{}. Same category, so `ec == std::error_code{}`
 * still holds.
 */
inline const std::error_code empty_error_code{0, std::system_category()};

}  // namespace bnio::ssl::base

#endif  // BNIO_SSL_BASE_ERRORS_H_
