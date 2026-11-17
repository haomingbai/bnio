/**
 * @file ssl_handshake_retry_category_peer.cpp
 * @brief Second translation unit for the handshake-pause category identity test.
 */

#include <bnio/ssl.h>

#include <system_error>

const std::error_category& handshake_pause_category_from_other_tu();

bool is_handshake_pause_from_other_tu(const std::error_code& ec);

bnio::ssl::handshake_pause_reason handshake_pause_reason_from_other_tu(
    const std::error_code& ec);

std::error_code make_handshake_pause_error_from_other_tu(int value);

const std::error_category& handshake_pause_category_from_other_tu() {
  return bnio::ssl::handshake_pause_category();
}

bool is_handshake_pause_from_other_tu(const std::error_code& ec) {
  return bnio::ssl::is_handshake_pause(ec);
}

bnio::ssl::handshake_pause_reason handshake_pause_reason_from_other_tu(
    const std::error_code& ec) {
  return bnio::ssl::handshake_pause_reason_of(ec);
}

std::error_code make_handshake_pause_error_from_other_tu(int value) {
  return std::error_code(value, bnio::ssl::handshake_pause_category());
}
