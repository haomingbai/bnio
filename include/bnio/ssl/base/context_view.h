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
 * Copies only the pointer; the SSL_CTX is freed by its RAII owner
 * (context_base via ssl::context), so the view is valid only while that owner
 * keeps it alive, and a context whose callbacks a stream will use must outlive
 * that stream. The view keeps no closure state; calls through an invalid view
 * are unsupported.
 */
class BNIO_EXPORT context_view {
 public:
  /** Native SSL_CTX handle type. */
  using native_handle_type = SSL_CTX*;

  /**
   * Matches SSL_CTX_set_alpn_select_cb; returns SSL_TLSEXT_ERR_*.
   */
  using alpn_select_cb = int (*)(SSL* ssl, const unsigned char** out,
                                 unsigned char* outlen, const unsigned char* in,
                                 unsigned int inlen, void* arg);

  /**
   * Matches SSL_CTX_set_tlsext_servername_callback; returns SSL_TLSEXT_ERR_*.
   */
  using servername_cb = int (*)(SSL* ssl, int* al, void* arg);

  /**
   * Matches SSL_CTX_set_client_hello_cb; returns SSL_CLIENT_HELLO_*.
   */
  using client_hello_cb = int (*)(SSL* ssl, int* al, void* arg);

  /**
   * Matches SSL_CTX_set_cert_cb; returns 1 (ok), 0 (abort), or -1 (pause with
   * SSL_ERROR_WANT_X509_LOOKUP).
   */
  using cert_cb = int (*)(SSL* ssl, void* arg);

  /** Creates an invalid view. */
  constexpr context_view() noexcept = default;

  /** Wraps a native SSL_CTX handle without taking ownership. */
  constexpr explicit context_view(native_handle_type context) noexcept
      : context_(context) {}

  /** Compares views by the wrapped handle. */
  [[nodiscard]] constexpr bool operator==(const context_view&) const noexcept =
      default;

  /** Returns the wrapped SSL_CTX, or nullptr when invalid. */
  [[nodiscard]] constexpr native_handle_type native_handle() const noexcept {
    return context_;
  }

  /** True when a native SSL_CTX is referenced. */
  [[nodiscard]] constexpr bool valid() const noexcept {
    return context_ != nullptr;
  }

  /** Loads a certificate chain file. */
  [[nodiscard]] std::error_code use_certificate_chain_file(
      const char* path) noexcept;

  /** Loads a PEM private key file. */
  [[nodiscard]] std::error_code use_private_key_file(const char* path) noexcept;

  /** Checks the private key against the certificate. */
  [[nodiscard]] std::error_code check_private_key() noexcept;

  /** Sets OpenSSL verification flags. */
  void set_verify_mode(int mode) noexcept;

  /**
   * Installs a native ALPN callback; arg stays caller-owned and must outlive
   * the native SSL_CTX's use of it. For owned closures use
   * ssl::context::set_alpn_callback instead.
   */
  void set_alpn_select_cb(alpn_select_cb cb, void* arg) noexcept;

  /**
   * Installs a native SNI servername callback; cb and the separately
   * registered arg stay caller-owned and must outlive the native SSL_CTX's
   * use of them. For owned closures use
   * ssl::context::set_servername_callback instead.
   */
  void set_tlsext_servername_callback(servername_cb cb) noexcept;

  /** Sets the SNI callback argument; caller-owned. */
  void set_tlsext_servername_arg(void* arg) noexcept;

  /**
   * Installs a native client_hello callback; cb and arg stay caller-owned and
   * must outlive the native SSL_CTX's use of them. For owned closures use
   * ssl::context::set_client_hello_callback instead.
   */
  void set_client_hello_cb(client_hello_cb cb, void* arg) noexcept;

  /**
   * Installs a native certificate callback; cb and arg stay caller-owned and
   * must outlive the native SSL_CTX's use of them. For owned closures use
   * ssl::context::set_cert_callback instead.
   */
  void set_cert_cb(cert_cb cb, void* arg) noexcept;

  /**
   * Installs an in-memory certificate (SSL_CTX_use_certificate), incrementing
   * its reference count; cert stays caller-owned. Returns empty on success,
   * else the recorded OpenSSL error.
   */
  [[nodiscard]] std::error_code use_certificate(X509* cert) noexcept;

  /**
   * Installs an in-memory private key (SSL_CTX_use_PrivateKey), incrementing
   * its reference count; key stays caller-owned. Returns empty on success,
   * else the recorded OpenSSL error.
   */
  [[nodiscard]] std::error_code use_private_key(EVP_PKEY* key) noexcept;

  /** Loads one certificate file (SSL_FILETYPE_PEM or ASN1). */
  [[nodiscard]] std::error_code use_certificate_file(const char* path,
                                                     int file_type) noexcept {
    clear_errors();
    if (SSL_CTX_use_certificate_file(context_, path, file_type) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /** Sets the maximum chain verification depth. */
  void set_verify_depth(int depth) noexcept {
    SSL_CTX_set_verify_depth(context_, depth);
  }

  /** Adds SSL_OP_* flags to the options already set. */
  void set_options(std::uint64_t options) noexcept {
    SSL_CTX_set_options(context_, options);
  }

  /** Returns the current SSL_OP_* flags. */
  [[nodiscard]] std::uint64_t get_options() const noexcept {
    return SSL_CTX_get_options(context_);
  }

  /** Sets the TLS 1.2 and earlier cipher list. */
  [[nodiscard]] std::error_code set_cipher_list(const char* lists) noexcept {
    clear_errors();
    if (SSL_CTX_set_cipher_list(context_, lists) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /** Sets the TLS 1.3 cipher suites. */
  [[nodiscard]] std::error_code set_ciphersuites(const char* suites) noexcept {
    clear_errors();
    if (SSL_CTX_set_ciphersuites(context_, suites) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /**
   * Loads trusted CAs from a file and/or directory; either may be nullptr.
   */
  [[nodiscard]] std::error_code load_verify_locations(
      const char* ca_file, const char* ca_dir) noexcept {
    clear_errors();
    if (SSL_CTX_load_verify_locations(context_, ca_file, ca_dir) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /** Loads the default trusted CA locations. */
  [[nodiscard]] std::error_code set_default_verify_paths() noexcept {
    clear_errors();
    if (SSL_CTX_set_default_verify_paths(context_) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /** Sets the minimum protocol version; 0 selects the default. */
  [[nodiscard]] std::error_code set_min_proto_version(int version) noexcept {
    clear_errors();
    if (SSL_CTX_set_min_proto_version(context_, version) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /** Sets the maximum protocol version; 0 selects the default. */
  [[nodiscard]] std::error_code set_max_proto_version(int version) noexcept {
    clear_errors();
    if (SSL_CTX_set_max_proto_version(context_, version) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /** Sets the session cache mode; the previous mode is ignored. */
  void set_session_cache_mode(long mode) noexcept {
    SSL_CTX_set_session_cache_mode(context_, mode);
  }

  /** Sets the server-side session resumption scope. */
  [[nodiscard]] std::error_code set_session_id_context(
      const unsigned char* sid, std::size_t length) noexcept {
    clear_errors();
    if (SSL_CTX_set_session_id_context(
            context_, sid, static_cast<unsigned int>(length)) == 1) {
      return empty_error_code;
    }
    return last_error();
  }

  /** Installs a TLS key material logger for SSLKEYLOGFILE debugging. */
  void set_keylog_callback(void (*cb)(const SSL* ssl,
                                      const char* line)) noexcept {
    SSL_CTX_set_keylog_callback(context_, cb);
  }

  /** Sets the client ALPN list in OpenSSL wire format (length-prefixed). */
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
