/**
 * @file stream_view.h
 * @brief Non-owning view over a native SSL connection.
 */

#pragma once
#ifndef BNIO_SSL_STREAM_VIEW_H_
#define BNIO_SSL_STREAM_VIEW_H_

#include <openssl/ssl.h>

#include <span>

namespace bnio::ssl {

/**
 * Non-owning view over a native OpenSSL SSL object.
 *
 * stream_view carries a connection handle across bnio interfaces (most
 * notably the ALPN selection callback installed by set_alpn_callback) without
 * transferring or implying ownership. Copying or moving the view copies only
 * the pointer: the SSL object remains owned and freed by its RAII owner
 * (ssl::tcp::stream), and the view stays valid only while that owner keeps the
 * connection alive.
 */
class stream_view {
 public:
  /**
   * Native SSL handle type.
   */
  using native_handle_type = SSL*;

  /**
   * Creates an invalid view.
   */
  constexpr stream_view() noexcept = default;

  /**
   * Wraps a native SSL handle without taking ownership.
   */
  constexpr explicit stream_view(native_handle_type ssl) noexcept : ssl_(ssl) {}

  /**
   * Compares views by the wrapped handle.
   */
  [[nodiscard]] constexpr bool operator==(const stream_view&) const noexcept =
      default;

  /**
   * Returns the wrapped native SSL handle, or nullptr when invalid.
   */
  [[nodiscard]] constexpr native_handle_type native_handle() const noexcept {
    return ssl_;
  }

  /**
   * Returns whether this view references a live SSL object.
   */
  [[nodiscard]] constexpr bool valid() const noexcept {
    return ssl_ != nullptr;
  }

  /**
   * Returns a non-owning read-only view of the ALPN protocol selected during
   * the handshake. The view is empty when no protocol was negotiated, when
   * the peer did not acknowledge ALPN, when the handshake has not completed,
   * or when this view is invalid. The bytes point into the SSL object's
   * internal storage and remain valid as long as the connection is alive;
   * renegotiation may change the contents.
   */
  [[nodiscard]] std::span<const unsigned char> alpn_selected() const noexcept {
    if (ssl_ == nullptr) {
      return {};
    }
    const unsigned char* data = nullptr;
    unsigned int len = 0;
    SSL_get0_alpn_selected(ssl_, &data, &len);
    return {data, len};
  }

  /**
   * Returns the TLS server name (SNI) carried by the connection, or nullptr
   * when none was sent or received. On a server this is the name the client
   * offered; on a client it is the name configured for the handshake.
   */
  [[nodiscard]] const char* servername() const noexcept {
    if (ssl_ == nullptr) {
      return nullptr;
    }
    return SSL_get_servername(ssl_, TLSEXT_NAMETYPE_host_name);
  }

 private:
  native_handle_type ssl_ = nullptr;
};

}  // namespace bnio::ssl

#endif  // BNIO_SSL_STREAM_VIEW_H_
