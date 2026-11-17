/**
 * @file ssl_handshake_retry_category_test.cpp
 * @brief Cross-translation-unit identity of the handshake pause category.
 *
 * The category object and make_handshake_pause_error() are defined out of line
 * in the bnio library so every translation unit and shared object observes the
 * same identity. One peer translation unit mints and classifies codes while
 * this one does the same from the other side; category identity is what makes
 * is_handshake_pause() work across that boundary.
 */

#include <bnio/ssl.h>
#include <gtest/gtest.h>

#include <string>
#include <system_error>

const std::error_category& handshake_pause_category_from_other_tu();

bool is_handshake_pause_from_other_tu(const std::error_code& ec);

bnio::ssl::handshake_pause_reason handshake_pause_reason_from_other_tu(
    const std::error_code& ec);

std::error_code make_handshake_pause_error_from_other_tu(int value);

namespace {

using bnio::ssl::handshake_pause_reason;

TEST(SslHandshakeRetryCategoryTest, category_identity_is_shared_across_tus) {
  const std::error_category& category = bnio::ssl::handshake_pause_category();

  // The peer translation unit resolves to the same category object.
  EXPECT_EQ(&handshake_pause_category_from_other_tu(), &category);
  EXPECT_EQ(std::string(handshake_pause_category_from_other_tu().name()),
            "bnio.ssl.handshake_pause");

  // A pause minted here is classified by the peer's inline helpers.
  const std::error_code pause = bnio::ssl::make_handshake_pause_error(
      handshake_pause_reason::want_write);
  EXPECT_TRUE(is_handshake_pause_from_other_tu(pause));
  EXPECT_EQ(handshake_pause_reason_from_other_tu(pause),
            handshake_pause_reason::want_write);

  // A pause minted by the peer is classified here.
  const std::error_code peer_pause = make_handshake_pause_error_from_other_tu(
      static_cast<int>(handshake_pause_reason::retry_verify));
  EXPECT_TRUE(bnio::ssl::is_handshake_pause(peer_pause));
  EXPECT_EQ(bnio::ssl::handshake_pause_reason_of(peer_pause),
            handshake_pause_reason::retry_verify);

  // Out-of-range values in the shared category have no reason on either side.
  const std::error_code unknown = make_handshake_pause_error_from_other_tu(99);
  EXPECT_TRUE(bnio::ssl::is_handshake_pause(unknown));
  EXPECT_EQ(bnio::ssl::handshake_pause_reason_of(unknown),
            handshake_pause_reason::none);
  EXPECT_EQ(handshake_pause_reason_from_other_tu(unknown),
            handshake_pause_reason::none);

  // Codes from another category are not pauses on either side.
  const std::error_code other =
      std::make_error_code(std::errc::permission_denied);
  EXPECT_FALSE(bnio::ssl::is_handshake_pause(other));
  EXPECT_FALSE(is_handshake_pause_from_other_tu(other));
  EXPECT_EQ(handshake_pause_reason_from_other_tu(other),
            handshake_pause_reason::none);

  // The peer's category constructor and this TU's classifier agree on the
  // empty code.
  const std::error_code empty =
      make_handshake_pause_error_from_other_tu(
          static_cast<int>(handshake_pause_reason::none));
  EXPECT_FALSE(empty);
  EXPECT_FALSE(is_handshake_pause_from_other_tu(empty));
}

}  // namespace
