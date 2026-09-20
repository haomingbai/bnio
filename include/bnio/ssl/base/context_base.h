/**
 * @file context_base.h
 * @brief RAII SSL_CTX owner base.
 */

#pragma once
#ifndef BNIO_SSL_BASE_CONTEXT_BASE_H_
#define BNIO_SSL_BASE_CONTEXT_BASE_H_

#include <bnio/export.h>
#include <bnio/ssl/base/alpn.h>
#include <bnio/ssl/base/errors.h>
#include <openssl/ssl.h>

#include <cstddef>
#include <cstdint>
#include <system_error>

namespace bnio::ssl::base {

/**
 * RAII owner for an OpenSSL SSL_CTX object.
 *
 * context_base owns the native context and frees it on destruction. It is
 * move-only because the native SSL_CTX ownership is unique.
 *
 * The constructor accepts an OpenSSL SSL_METHOD directly. Translating bnio
 * method enumerators into SSL_METHOD values is a public-layer concern; the
 * base only receives the native OpenSSL method, which keeps this type free of
 * bnio transport vocabulary and lets future transports (e.g. QUIC) build
 * contexts from their own methods.
 */
class BNIO_EXPORT context_base {
 public:
  /**
   * Creates an SSL context from a native OpenSSL method. If SSL_CTX_new
   * fails, the context is left invalid.
   */
  explicit context_base(const SSL_METHOD* method) noexcept;

  /**
   * Frees the owned SSL_CTX, if any.
   */
  ~context_base() noexcept;

  /**
   * Copy construction is disabled because the context owns an SSL_CTX.
   */
  context_base(const context_base&) = delete;

  /**
   * Copy assignment is disabled because the context owns an SSL_CTX.
   */
  context_base& operator=(const context_base&) = delete;

  /**
   * Moves SSL_CTX ownership from another context.
   */
  context_base(context_base&& other) noexcept;

  /**
   * Frees the current SSL_CTX and moves ownership from another context.
   */
  context_base& operator=(context_base&& other) noexcept;

  /**
   * Returns the owned native SSL_CTX pointer, or nullptr when invalid.
   */
  [[nodiscard]] SSL_CTX* native_handle() const noexcept { return context_; }

  /**
   * Returns whether this context owns a native SSL_CTX.
   */
  [[nodiscard]] bool valid() const noexcept { return context_ != nullptr; }

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
   * Callback signature matching OpenSSL's SSL_CTX_set_alpn_select_cb
   * protocol. Returns SSL_TLSEXT_ERR_OK, SSL_TLSEXT_ERR_NOACK,
   * SSL_TLSEXT_ERR_ALERT_FATAL, etc.
   */
  using alpn_select_cb = int (*)(SSL* ssl, const unsigned char** out,
                                 unsigned char* outlen, const unsigned char* in,
                                 unsigned int inlen, void* arg);

  /**
   * Installs an ALPN selection callback. The context does not own arg; the
   * caller must keep it alive for the lifetime of the context.
   */
  void set_alpn_select_cb(alpn_select_cb cb, void* arg) noexcept;

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
  void set_keylog_callback(
      void (*cb)(const SSL* ssl, const char* line)) noexcept {
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

 protected:
  /**
   * Takes ownership of owned_arg, destroying any previously owned argument.
   */
  void store_owned_alpn_arg(alpn_arg_base* owned_arg) noexcept;

 private:
  SSL_CTX* context_ = nullptr;
  alpn_arg_base* owned_alpn_arg_ = nullptr;
};

}  // namespace bnio::ssl::base

#endif  // BNIO_SSL_BASE_CONTEXT_BASE_H_
