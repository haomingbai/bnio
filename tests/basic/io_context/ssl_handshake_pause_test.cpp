/**
 * @file ssl_handshake_pause_test.cpp
 * @brief Basic unit tests for the SSL handshake pause category and table.
 *
 * These tests include detail headers on purpose: the pause table is an
 * internal classification source, not public API.
 */

#include <bnio/ssl/base/errors.h>
#include <bnio/ssl/detail/handshake_pause_table.h>
#include <bnio/ssl/handshake_pause.h>
#include <gtest/gtest.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <system_error>

namespace {

using bnio::ssl::handshake_pause_category;
using bnio::ssl::handshake_pause_reason;
using bnio::ssl::handshake_pause_reason_of;
using bnio::ssl::is_handshake_pause;
using bnio::ssl::make_handshake_pause_error;

constexpr handshake_pause_reason k_pause_reasons[] = {
    handshake_pause_reason::want_read,
    handshake_pause_reason::want_write,
    handshake_pause_reason::x509_lookup,
    handshake_pause_reason::async,
    handshake_pause_reason::async_job,
    handshake_pause_reason::client_hello_cb,
    handshake_pause_reason::retry_verify,
};

TEST(SslHandshakePauseTest, category_identity) {
  const std::error_category& category = handshake_pause_category();
  EXPECT_EQ(std::string(category.name()), "bnio.ssl.handshake_pause");
  EXPECT_EQ(&category, &handshake_pause_category());
}

TEST(SslHandshakePauseTest, none_maps_to_empty_error_code) {
  const std::error_code ec =
      make_handshake_pause_error(handshake_pause_reason::none);
  EXPECT_EQ(ec, std::error_code{});
  EXPECT_FALSE(ec);
  EXPECT_EQ(ec.value(), 0);
  EXPECT_FALSE(is_handshake_pause(ec));
  EXPECT_EQ(handshake_pause_reason_of(ec), handshake_pause_reason::none);
  // Even the empty code reports a non-empty message.
  EXPECT_FALSE(ec.message().empty());
}

TEST(SslHandshakePauseTest, every_reason_mints_a_distinct_pause_code) {
  std::array<std::error_code, std::size(k_pause_reasons)> codes{};
  for (std::size_t index = 0; index < std::size(k_pause_reasons); ++index) {
    const handshake_pause_reason reason = k_pause_reasons[index];
    const std::error_code ec = make_handshake_pause_error(reason);
    EXPECT_TRUE(ec);
    EXPECT_NE(ec, std::error_code{});
    EXPECT_EQ(ec.category(), handshake_pause_category());
    EXPECT_EQ(ec.value(), static_cast<int>(reason));
    EXPECT_TRUE(is_handshake_pause(ec));
    EXPECT_EQ(handshake_pause_reason_of(ec), reason);
    codes[index] = ec;
  }

  for (std::size_t left = 0; left < codes.size(); ++left) {
    for (std::size_t right = left + 1; right < codes.size(); ++right) {
      EXPECT_NE(codes[left].value(), codes[right].value());
      EXPECT_NE(codes[left], codes[right]);
    }
  }
}

TEST(SslHandshakePauseTest, messages_are_nonempty_and_stable_for_every_value) {
  const handshake_pause_reason all_reasons[] = {
      handshake_pause_reason::none,
      handshake_pause_reason::want_read,
      handshake_pause_reason::want_write,
      handshake_pause_reason::x509_lookup,
      handshake_pause_reason::async,
      handshake_pause_reason::async_job,
      handshake_pause_reason::client_hello_cb,
      handshake_pause_reason::retry_verify,
  };
  for (const handshake_pause_reason reason : all_reasons) {
    const std::string message = make_handshake_pause_error(reason).message();
    EXPECT_FALSE(message.empty());
    EXPECT_EQ(make_handshake_pause_error(reason).message(), message);
  }

  // Value zero inside the pause category has its own message even though it
  // is not a pause.
  const std::error_code zero_in_category{0, handshake_pause_category()};
  EXPECT_FALSE(zero_in_category.message().empty());

  // Unknown values, including negative and out-of-range ones, still report a
  // non-empty stable message.
  const int unknown_values[] = {-1, 8, 77, std::numeric_limits<int>::max()};
  for (const int value : unknown_values) {
    const std::error_code ec(value, handshake_pause_category());
    const std::string message = ec.message();
    EXPECT_FALSE(message.empty());
    EXPECT_EQ(ec.message(), message);
  }
}

TEST(SslHandshakePauseTest, non_pause_errors_are_not_pauses) {
  // The empty code.
  EXPECT_FALSE(is_handshake_pause(std::error_code{}));
  EXPECT_EQ(handshake_pause_reason_of(std::error_code{}),
            handshake_pause_reason::none);

  // The dedicated no-OpenSSL-error value.
  const std::error_code no_ssl_error = bnio::ssl::base::make_no_ssl_error();
  EXPECT_TRUE(no_ssl_error);
  EXPECT_FALSE(is_handshake_pause(no_ssl_error));
  EXPECT_EQ(handshake_pause_reason_of(no_ssl_error),
            handshake_pause_reason::none);

  // An error genuinely recorded by OpenSSL's thread-local queue.
  ERR_clear_error();
  ERR_raise(ERR_LIB_SSL, ERR_R_INTERNAL_ERROR);
  const unsigned long raw_error = ERR_get_error();
  ASSERT_NE(raw_error, 0UL);
  const std::error_code openssl_error =
      bnio::ssl::base::make_openssl_error(raw_error);
  EXPECT_EQ(openssl_error.category(),
            bnio::ssl::base::openssl_error_category());
  EXPECT_TRUE(openssl_error);
  EXPECT_FALSE(is_handshake_pause(openssl_error));
  EXPECT_EQ(handshake_pause_reason_of(openssl_error),
            handshake_pause_reason::none);
  // A synthetic non-zero OpenSSL-category value is not a pause either.
  EXPECT_FALSE(is_handshake_pause(
      bnio::ssl::base::make_openssl_error(static_cast<unsigned long>(1))));

  // std::errc codes never belong to the pause category.
  const std::error_code permission_denied =
      std::make_error_code(std::errc::permission_denied);
  EXPECT_FALSE(is_handshake_pause(permission_denied));
  EXPECT_EQ(handshake_pause_reason_of(permission_denied),
            handshake_pause_reason::none);

  // Zero in the pause category is not a pause.
  const std::error_code zero_in_category{0, handshake_pause_category()};
  EXPECT_FALSE(is_handshake_pause(zero_in_category));
  EXPECT_EQ(handshake_pause_reason_of(zero_in_category),
            handshake_pause_reason::none);
}

TEST(SslHandshakePauseTest, out_of_range_pause_values_have_no_reason) {
  const int out_of_range_values[] = {
      -1,
      8,
      77,
      std::numeric_limits<int>::max(),
  };
  for (const int value : out_of_range_values) {
    const std::error_code ec(value, handshake_pause_category());
    // Category identity plus a nonzero value still makes it a pause code;
    // the reason is what is unknown.
    EXPECT_TRUE(is_handshake_pause(ec));
    EXPECT_EQ(handshake_pause_reason_of(ec), handshake_pause_reason::none);
  }
}

// The public enum values are fixed by contract and unrelated to SSL_ERROR_*.
static_assert(static_cast<int>(handshake_pause_reason::none) == 0);
static_assert(static_cast<int>(handshake_pause_reason::want_read) == 1);
static_assert(static_cast<int>(handshake_pause_reason::want_write) == 2);
static_assert(static_cast<int>(handshake_pause_reason::x509_lookup) == 3);
static_assert(static_cast<int>(handshake_pause_reason::async) == 4);
static_assert(static_cast<int>(handshake_pause_reason::async_job) == 5);
static_assert(static_cast<int>(handshake_pause_reason::client_hello_cb) == 6);
static_assert(static_cast<int>(handshake_pause_reason::retry_verify) == 7);

struct expected_pause_entry {
  int ssl_error;
  handshake_pause_reason reason;
};

// The complete expected contents of the table, written independently of the
// table itself so a missing or mis-mapped row fails the tests.
constexpr expected_pause_entry k_expected_pause_entries[] = {
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

TEST(SslHandshakePauseTableTest, table_is_sorted_by_ssl_error) {
  using bnio::ssl::detail::handshake_pause_table;
  ASSERT_GE(std::size(handshake_pause_table), 2U);
  for (std::size_t index = 1; index < std::size(handshake_pause_table);
       ++index) {
    EXPECT_LT(handshake_pause_table[index - 1].ssl_error,
              handshake_pause_table[index].ssl_error)
        << "entry " << index << " breaks the required ascending order";
  }
}

TEST(SslHandshakePauseTableTest, table_contains_exactly_the_expected_entries) {
  using bnio::ssl::detail::handshake_pause_table;
  ASSERT_EQ(std::size(k_expected_pause_entries),
            std::size(handshake_pause_table));
  for (const expected_pause_entry& expected : k_expected_pause_entries) {
    const auto* const entry = std::find_if(
        std::begin(handshake_pause_table), std::end(handshake_pause_table),
        [&expected](const auto& candidate) noexcept {
          return candidate.ssl_error == expected.ssl_error;
        });
    ASSERT_NE(entry, std::end(handshake_pause_table))
        << "missing table entry for SSL error " << expected.ssl_error;
    EXPECT_EQ(entry->reason, expected.reason);
  }
}

TEST(SslHandshakePauseTableTest, every_table_entry_maps_to_its_reason) {
  using bnio::ssl::detail::handshake_pause_table;
  using bnio::ssl::detail::lookup_handshake_pause;
  for (const auto& entry : handshake_pause_table) {
    const std::optional<handshake_pause_reason> reason =
        lookup_handshake_pause(entry.ssl_error);
    ASSERT_TRUE(reason.has_value())
        << "SSL error " << entry.ssl_error << " is not classified as a pause";
    EXPECT_EQ(*reason, entry.reason);
  }

  static_assert(lookup_handshake_pause(SSL_ERROR_WANT_READ) ==
                handshake_pause_reason::want_read);
  static_assert(lookup_handshake_pause(SSL_ERROR_WANT_WRITE) ==
                handshake_pause_reason::want_write);
}

TEST(SslHandshakePauseTableTest, non_entries_are_not_pauses) {
  using bnio::ssl::detail::lookup_handshake_pause;
  EXPECT_FALSE(lookup_handshake_pause(SSL_ERROR_ZERO_RETURN).has_value());
  EXPECT_FALSE(lookup_handshake_pause(SSL_ERROR_SSL).has_value());
  EXPECT_FALSE(lookup_handshake_pause(0).has_value());
  EXPECT_FALSE(lookup_handshake_pause(-1).has_value());
  EXPECT_FALSE(lookup_handshake_pause(1000).has_value());
  EXPECT_FALSE(
      lookup_handshake_pause(std::numeric_limits<int>::max()).has_value());
  EXPECT_FALSE(
      lookup_handshake_pause(std::numeric_limits<int>::min()).has_value());
#ifdef SSL_ERROR_WANT_CONNECT
  // Transport connect-state results are not handshake progress pauses.
  EXPECT_FALSE(lookup_handshake_pause(SSL_ERROR_WANT_CONNECT).has_value());
#endif
#ifdef SSL_ERROR_WANT_ACCEPT
  EXPECT_FALSE(lookup_handshake_pause(SSL_ERROR_WANT_ACCEPT).has_value());
#endif

  static_assert(!lookup_handshake_pause(SSL_ERROR_ZERO_RETURN).has_value());
}

}  // namespace
