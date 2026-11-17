/**
 * @file context_base.h
 * @brief RAII SSL_CTX owner base.
 */

#pragma once
#ifndef BNIO_SSL_BASE_CONTEXT_BASE_H_
#define BNIO_SSL_BASE_CONTEXT_BASE_H_

#include <bnio/export.h>
#include <bnio/ssl/base/context_view.h>
#include <openssl/ssl.h>

#include <cstddef>
#include <cstdint>
#include <system_error>

namespace bnio::ssl::base {

/**
 * RAII owner for an OpenSSL SSL_CTX object.
 *
 * context_base owns the native context and frees it on destruction. It is
 * move-only because the native SSL_CTX ownership is unique. Moving transfers
 * only the handle; callbacks registered on the native SSL_CTX stay registered
 * and continue to point at whatever state their installers own.
 *
 * The owner also keeps the non-owning configuration and query surface for
 * callers that work with a context_base directly: every configuration and
 * query method forwards inline to view(), the non-owning context_view over
 * the same native handle. context_view is the only implementation of that
 * surface; this type and ssl::context only route to it. The owner itself
 * keeps no callback state: ssl::context layers owned callback closures on
 * top.
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
   * Frees the owned SSL_CTX, if any. Callbacks registered on the native
   * SSL_CTX are not cleared here; an owner that installed closures must clear
   * the native registrations before destroying them.
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
   * Returns a non-owning view over the owned native SSL_CTX, or an invalid
   * view when this context owns no SSL_CTX. The view is valid only while this
   * owner keeps the handle alive.
   */
  [[nodiscard]] context_view view() const noexcept {
    return context_view(context_);
  }

  /**
   * Native ALPN selection callback signature re-exported from context_view.
   */
  using alpn_select_cb = context_view::alpn_select_cb;

  /**
   * Loads a certificate chain file into the context. Forwards to view().
   */
  [[nodiscard]] std::error_code use_certificate_chain_file(
      const char* path) noexcept {
    return view().use_certificate_chain_file(path);
  }

  /**
   * Loads a PEM private key file into the context. Forwards to view().
   */
  [[nodiscard]] std::error_code use_private_key_file(
      const char* path) noexcept {
    return view().use_private_key_file(path);
  }

  /**
   * Checks whether the loaded private key matches the certificate. Forwards
   * to view().
   */
  [[nodiscard]] std::error_code check_private_key() noexcept {
    return view().check_private_key();
  }

  /**
   * Sets OpenSSL certificate verification flags for the context. Forwards to
   * view().
   */
  void set_verify_mode(int mode) noexcept { view().set_verify_mode(mode); }

  /**
   * Installs a native ALPN selection callback. The context does not own arg;
   * the caller must keep it alive for as long as the native SSL_CTX may
   * invoke the callback. Forwards to view().
   */
  void set_alpn_select_cb(alpn_select_cb cb, void* arg) noexcept {
    view().set_alpn_select_cb(cb, arg);
  }

  /**
   * Loads a single certificate file (SSL_FILETYPE_PEM or SSL_FILETYPE_ASN1).
   * Forwards to view().
   */
  [[nodiscard]] std::error_code use_certificate_file(const char* path,
                                                     int file_type) noexcept {
    return view().use_certificate_file(path, file_type);
  }

  /**
   * Sets the maximum depth of the certificate chain verification. Forwards to
   * view().
   */
  void set_verify_depth(int depth) noexcept { view().set_verify_depth(depth); }

  /**
   * Sets SSL/TLS option flags (SSL_OP_*); the previous options are ignored.
   * Forwards to view().
   */
  void set_options(std::uint64_t options) noexcept {
    view().set_options(options);
  }

  /**
   * Returns the current SSL/TLS option flags. Forwards to view().
   */
  [[nodiscard]] std::uint64_t get_options() const noexcept {
    return view().get_options();
  }

  /**
   * Sets the cipher list for TLS 1.2 and earlier. Forwards to view().
   */
  [[nodiscard]] std::error_code set_cipher_list(const char* lists) noexcept {
    return view().set_cipher_list(lists);
  }

  /**
   * Sets the cipher suites for TLS 1.3. Forwards to view().
   */
  [[nodiscard]] std::error_code set_ciphersuites(const char* suites) noexcept {
    return view().set_ciphersuites(suites);
  }

  /**
   * Loads trusted CA certificates from a file and/or directory; either
   * argument may be nullptr. Forwards to view().
   */
  [[nodiscard]] std::error_code load_verify_locations(
      const char* ca_file, const char* ca_dir) noexcept {
    return view().load_verify_locations(ca_file, ca_dir);
  }

  /**
   * Loads the default trusted CA locations. Forwards to view().
   */
  [[nodiscard]] std::error_code set_default_verify_paths() noexcept {
    return view().set_default_verify_paths();
  }

  /**
   * Sets the minimum protocol version (0 selects the automatic default).
   * Forwards to view().
   */
  [[nodiscard]] std::error_code set_min_proto_version(int version) noexcept {
    return view().set_min_proto_version(version);
  }

  /**
   * Sets the maximum protocol version (0 selects the automatic default).
   * Forwards to view().
   */
  [[nodiscard]] std::error_code set_max_proto_version(int version) noexcept {
    return view().set_max_proto_version(version);
  }

  /**
   * Sets the session cache mode; the previous mode is ignored. Forwards to
   * view().
   */
  void set_session_cache_mode(long mode) noexcept {
    view().set_session_cache_mode(mode);
  }

  /**
   * Sets the session id context used by servers to scope session resumption.
   * Forwards to view().
   */
  [[nodiscard]] std::error_code set_session_id_context(
      const unsigned char* sid, std::size_t length) noexcept {
    return view().set_session_id_context(sid, length);
  }

  /**
   * Installs a TLS key material logging callback for SSLKEYLOGFILE-style
   * debugging (e.g. decrypting captures in an external analyzer). Forwards to
   * view().
   */
  void set_keylog_callback(void (*cb)(const SSL* ssl,
                                      const char* line)) noexcept {
    view().set_keylog_callback(cb);
  }

  /**
   * Sets the client-side ALPN protocol list in OpenSSL wire format: a
   * sequence of protocol names, each prefixed by a single length byte.
   * Forwards to view().
   */
  [[nodiscard]] std::error_code set_alpn_protos(const unsigned char* data,
                                                unsigned int length) noexcept {
    return view().set_alpn_protos(data, length);
  }

 private:
  SSL_CTX* context_ = nullptr;
};

}  // namespace bnio::ssl::base

#endif  // BNIO_SSL_BASE_CONTEXT_BASE_H_
