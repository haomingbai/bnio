/**
 * @file context_view.h
 * @brief Non-owning view over a native OpenSSL SSL_CTX.
 */

#pragma once
#ifndef BNIO_SSL_BASE_CONTEXT_VIEW_H_
#define BNIO_SSL_BASE_CONTEXT_VIEW_H_

#include <bnio/export.h>
#include <bnio/ssl/base/errors.h>
#include <openssl/ssl.h>

#include <cstddef>
#include <cstdint>
#include <system_error>

namespace bnio::ssl::base {

/**
 * Non-owning view over a native OpenSSL SSL_CTX.
 *
 * context_view carries a native context handle across bnio interfaces without
 * transferring or implying ownership. Copying or moving the view copies only
 * the pointer: the SSL_CTX remains owned and freed by its RAII owner
 * (ssl::base::context_base, normally through ssl::context). The view is valid
 * only while that owner keeps the native SSL_CTX alive; in particular, a
 * context must outlive every ssl::tcp::stream whose handshake should still see
 * the context's ALPN callback.
 *
 * The view exposes the non-owning configuration and query surface of the
 * context. It never installs a closure: the high-level ALPN callback lives on
 * ssl::context, not here. Calls through an invalid view reach OpenSSL with a
 * null SSL_CTX and are not supported.
 */
class BNIO_EXPORT context_view {
 public:
  /**
   * Native SSL_CTX handle type.
   */
  using native_handle_type = SSL_CTX*;

  /**
   * Callback signature matching OpenSSL's SSL_CTX_set_alpn_select_cb
   * protocol. Returns SSL_TLSEXT_ERR_OK, SSL_TLSEXT_ERR_NOACK,
   * SSL_TLSEXT_ERR_ALERT_FATAL, etc.
   */
  using alpn_select_cb = int (*)(SSL* ssl, const unsigned char** out,
                                 unsigned char* outlen, const unsigned char* in,
                                 unsigned int inlen, void* arg);

  /**
   * Callback signature matching OpenSSL's SNI servername callback protocol
   * (SSL_CTX_set_tlsext_servername_callback). Returns SSL_TLSEXT_ERR_OK,
   * SSL_TLSEXT_ERR_NOACK, SSL_TLSEXT_ERR_ALERT_FATAL, etc.
   */
  using servername_cb = int (*)(SSL* ssl, int* al, void* arg);

  /**
   * Callback signature matching OpenSSL's client_hello callback protocol
   * (SSL_CTX_set_client_hello_cb). Returns SSL_CLIENT_HELLO_SUCCESS,
   * SSL_CLIENT_HELLO_ERROR, or SSL_CLIENT_HELLO_RETRY.
   */
  using client_hello_cb = int (*)(SSL* ssl, int* al, void* arg);

  /**
   * Callback signature matching OpenSSL's certificate callback protocol
   * (SSL_CTX_set_cert_cb). Returns 1 on success, 0 to abort the handshake, or
   * -1 to pause with SSL_ERROR_WANT_X509_LOOKUP.
   */
  using cert_cb = int (*)(SSL* ssl, void* arg);

  /**
   * Creates an invalid view.
   */
  constexpr context_view() noexcept = default;

  /**
   * Wraps a native SSL_CTX handle without taking ownership.
   */
  constexpr explicit context_view(native_handle_type context) noexcept
      : context_(context) {}

  /**
   * Compares views by the wrapped handle.
   */
  [[nodiscard]] constexpr bool operator==(const context_view&) const noexcept =
      default;

  /**
   * Returns the wrapped native SSL_CTX pointer, or nullptr when invalid.
   */
  [[nodiscard]] constexpr native_handle_type native_handle() const noexcept {
    return context_;
  }

  /**
   * Returns whether this view references a native SSL_CTX.
   */
  [[nodiscard]] constexpr bool valid() const noexcept {
    return context_ != nullptr;
  }

  /**
   * Loads a certificate chain file into the context.
   */
  [[nodiscard]] std::error_code use_certificate_chain_file(
      const char* path) noexcept;

  /**
   * Loads a PEM private key file into the context.
   */
  [[nodiscard]] std::error_code use_private_key_file(const char* path) noexcept;

  /**
   * Checks whether the loaded private key matches the certificate.
   */
  [[nodiscard]] std::error_code check_private_key() noexcept;

  /**
   * Sets OpenSSL certificate verification flags for the context.
   */
  void set_verify_mode(int mode) noexcept;

  /**
   * Installs a native ALPN selection callback. The view does not own arg; the
   * caller must keep it alive for as long as the native SSL_CTX may invoke
   * the callback. High-level callbacks with owned closure state are installed
   * through ssl::context::set_alpn_callback instead.
   */
  void set_alpn_select_cb(alpn_select_cb cb, void* arg) noexcept;

  /**
   * Installs a native SNI servername callback on the SSL_CTX. The view does
   * not own cb or the argument registered separately through
   * set_tlsext_servername_arg; the caller must keep them valid for as long as
   * the native SSL_CTX may invoke the callback. High-level callbacks with
   * owned closure state are installed through
   * ssl::context::set_servername_callback instead.
   */
  void set_tlsext_servername_callback(servername_cb cb) noexcept;

  /**
   * Sets the opaque argument OpenSSL passes to the SNI servername callback
   * registered on this SSL_CTX. The view does not own arg.
   */
  void set_tlsext_servername_arg(void* arg) noexcept;

  /**
   * Installs a native client_hello callback on the SSL_CTX. The view does not
   * own cb or arg; the caller must keep them valid for as long as the native
   * SSL_CTX may invoke the callback. High-level callbacks with owned closure
   * state are installed through ssl::context::set_client_hello_callback
   * instead.
   */
  void set_client_hello_cb(client_hello_cb cb, void* arg) noexcept;

  /**
   * Installs a native certificate callback on the SSL_CTX. The view does not
   * own cb or arg; the caller must keep them valid for as long as the native
   * SSL_CTX may invoke the callback. High-level callbacks with owned closure
   * state are installed through ssl::context::set_cert_callback instead.
   */
  void set_cert_cb(cert_cb cb, void* arg) noexcept;

  /**
   * Installs an in-memory certificate on the SSL_CTX, mirroring
   * SSL_CTX_use_certificate. OpenSSL increments the certificate reference
   * count; the caller keeps ownership of cert. Returns an empty error_code on
   * success and the OpenSSL error recorded by the failed call otherwise.
   */
  [[nodiscard]] std::error_code use_certificate(X509* cert) noexcept;

  /**
   * Installs an in-memory private key on the SSL_CTX, mirroring
   * SSL_CTX_use_PrivateKey. OpenSSL increments the key reference count; the
   * caller keeps ownership of key. Returns an empty error_code on success and
   * the OpenSSL error recorded by the failed call otherwise.
   */
  [[nodiscard]] std::error_code use_private_key(EVP_PKEY* key) noexcept;

  /**
   * Loads a single certificate file (SSL_FILETYPE_PEM or SSL_FILETYPE_ASN1).
   */
  [[nodiscard]] std::error_code use_certificate_file(const char* path,
                                                     int file_type) noexcept {
    clear_errors();
    if (SSL_CTX_use_certificate_file(context_, path, file_type) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /**
   * Sets the maximum depth of the certificate chain verification.
   */
  void set_verify_depth(int depth) noexcept {
    SSL_CTX_set_verify_depth(context_, depth);
  }

  /**
   * Sets SSL/TLS option flags (SSL_OP_*); the previous options are ignored.
   */
  void set_options(std::uint64_t options) noexcept {
    SSL_CTX_set_options(context_, options);
  }

  /**
   * Returns the current SSL/TLS option flags.
   */
  [[nodiscard]] std::uint64_t get_options() const noexcept {
    return SSL_CTX_get_options(context_);
  }

  /**
   * Sets the cipher list for TLS 1.2 and earlier.
   */
  [[nodiscard]] std::error_code set_cipher_list(const char* lists) noexcept {
    clear_errors();
    if (SSL_CTX_set_cipher_list(context_, lists) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /**
   * Sets the cipher suites for TLS 1.3.
   */
  [[nodiscard]] std::error_code set_ciphersuites(const char* suites) noexcept {
    clear_errors();
    if (SSL_CTX_set_ciphersuites(context_, suites) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /**
   * Loads trusted CA certificates from a file and/or directory; either
   * argument may be nullptr.
   */
  [[nodiscard]] std::error_code load_verify_locations(
      const char* ca_file, const char* ca_dir) noexcept {
    clear_errors();
    if (SSL_CTX_load_verify_locations(context_, ca_file, ca_dir) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /**
   * Loads the default trusted CA locations.
   */
  [[nodiscard]] std::error_code set_default_verify_paths() noexcept {
    clear_errors();
    if (SSL_CTX_set_default_verify_paths(context_) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /**
   * Sets the minimum protocol version (0 selects the automatic default).
   */
  [[nodiscard]] std::error_code set_min_proto_version(int version) noexcept {
    clear_errors();
    if (SSL_CTX_set_min_proto_version(context_, version) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /**
   * Sets the maximum protocol version (0 selects the automatic default).
   */
  [[nodiscard]] std::error_code set_max_proto_version(int version) noexcept {
    clear_errors();
    if (SSL_CTX_set_max_proto_version(context_, version) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /**
   * Sets the session cache mode; the previous mode is ignored.
   */
  void set_session_cache_mode(long mode) noexcept {
    SSL_CTX_set_session_cache_mode(context_, mode);
  }

  /**
   * Sets the session id context used by servers to scope session resumption.
   */
  [[nodiscard]] std::error_code set_session_id_context(
      const unsigned char* sid, std::size_t length) noexcept {
    clear_errors();
    if (SSL_CTX_set_session_id_context(
            context_, sid, static_cast<unsigned int>(length)) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /**
   * Installs a TLS key material logging callback for SSLKEYLOGFILE-style
   * debugging (e.g. decrypting captures in an external analyzer).
   */
  void set_keylog_callback(void (*cb)(const SSL* ssl,
                                      const char* line)) noexcept {
    SSL_CTX_set_keylog_callback(context_, cb);
  }

  /**
   * Sets the client-side ALPN protocol list in OpenSSL wire format: a
   * sequence of protocol names, each prefixed by a single length byte.
   */
  [[nodiscard]] std::error_code set_alpn_protos(const unsigned char* data,
                                                unsigned int length) noexcept {
    // Unlike the surrounding SSL_CTX calls, SSL_CTX_set_alpn_protos reports
    // success with 0.
    clear_errors();
    if (SSL_CTX_set_alpn_protos(context_, data, length) == 0) {
      return empty_error_code;
    }
    return last_error();
  }

 private:
  native_handle_type context_ = nullptr;
};

}  // namespace bnio::ssl::base

#endif  // BNIO_SSL_BASE_CONTEXT_VIEW_H_
