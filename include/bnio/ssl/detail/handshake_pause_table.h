/**
 * @file handshake_pause_table.h
 * @brief Sorted classification of SSL_ERROR_* results as handshake pauses.
 */

#pragma once
#ifndef BNIO_SSL_DETAIL_HANDSHAKE_PAUSE_TABLE_H_
#define BNIO_SSL_DETAIL_HANDSHAKE_PAUSE_TABLE_H_

#include <bnio/ssl/handshake_pause.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>

namespace bnio::ssl {
/** @cond BNIO_DETAIL */
namespace detail {

/**
 * One row: an SSL_get_error() result and the pause reason it represents.
 */
struct handshake_pause_entry {
  int ssl_error;
  handshake_pause_reason reason;
};

/**
 * SSL_get_error() results that mean "handshake paused", in ascending ssl_error
 * order (enforced by static_assert) for the binary search; optional WANT_*
 * macros are ifdef-guarded. ZERO_RETURN/WANT_CONNECT/WANT_ACCEPT are terminal
 * or transport-connect and intentionally absent.
 */
inline constexpr handshake_pause_entry handshake_pause_table[] = {
    {SSL_ERROR_WANT_READ, handshake_pause_reason::want_read},
    {SSL_ERROR_WANT_WRITE, handshake_pause_reason::want_write},
#ifdef SSL_ERROR_WANT_X509_LOOKUP
    {SSL_ERROR_WANT_X509_LOOKUP, handshake_pause_reason::x509_lookup},
#endif
#ifdef SSL_ERROR_WANT_ASYNC
    {SSL_ERROR_WANT_ASYNC, handshake_pause_reason::async},
#endif
#ifdef SSL_ERROR_WANT_ASYNC_JOB
    {SSL_ERROR_WANT_ASYNC_JOB, handshake_pause_reason::async_job},
#endif
#ifdef SSL_ERROR_WANT_CLIENT_HELLO_CB
    {SSL_ERROR_WANT_CLIENT_HELLO_CB, handshake_pause_reason::client_hello_cb},
#endif
#ifdef SSL_ERROR_WANT_RETRY_VERIFY
    {SSL_ERROR_WANT_RETRY_VERIFY, handshake_pause_reason::retry_verify},
#endif
};

namespace handshake_pause_table_detail {

// Returns whether entries[0, count) is strictly increasing by ssl_error.
[[nodiscard]] constexpr bool entries_sorted(
    const handshake_pause_entry* entries, std::size_t count) noexcept {
  for (std::size_t index = 1; index < count; ++index) {
    if (!(entries[index - 1].ssl_error < entries[index].ssl_error)) {
      return false;
    }
  }
  return true;
}

}  // namespace handshake_pause_table_detail

static_assert(handshake_pause_table_detail::entries_sorted(
                  handshake_pause_table, std::size(handshake_pause_table)),
              "handshake_pause_table must stay sorted by ascending ssl_error; "
              "insert new entries in order");

/**
 * Classifies an SSL_get_error() result.
 *
 * Returns the matching pause reason, or std::nullopt for hard failures and
 * non-pause results.
 */
[[nodiscard]] constexpr std::optional<handshake_pause_reason>
lookup_handshake_pause(int ssl_error) noexcept {
  const auto* const first = std::begin(handshake_pause_table);
  const auto* const last = std::end(handshake_pause_table);
  const auto* const entry = std::lower_bound(
      first, last, ssl_error,
      [](const handshake_pause_entry& candidate, int value) noexcept {
        return candidate.ssl_error < value;
      });
  if (entry == last || entry->ssl_error != ssl_error) {
    return std::nullopt;
  }
  return entry->reason;
}

}  // namespace detail
/** @endcond */
}  // namespace bnio::ssl

#endif  // BNIO_SSL_DETAIL_HANDSHAKE_PAUSE_TABLE_H_
