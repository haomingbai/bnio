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
 * Non-owning view over a native OpenSSL SSL object; copying copies only the
 * pointer. The SSL object is owned and freed by ssl::tcp::stream, and the view
 * is valid only while that owner lives. The SSL_CTX and callback closures are
 * owned by ssl::context, which must stay alive for the ALPN callback to remain
 * installed; destroying it clears the registrations even while the connection
 * itself is still alive.
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
   * Non-owning view of the ALPN protocol selected during the handshake; empty
   * when none was negotiated, the handshake has not completed, or this view is
   * invalid. The bytes point into the SSL object and stay valid while the
   * connection lives; renegotiation may change the contents.
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
   * Returns the TLS SNI name, or nullptr when none was sent or received. On a
   * server this is the name the client offered; on a client, the name
   * configured for the handshake.
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
