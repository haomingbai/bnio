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
 * RAII owner for an OpenSSL SSL_CTX; move-only, unique ownership.
 *
 * Moving transfers only the handle: native callbacks stay registered and keep
 * pointing at state their installers own. Config and query methods forward to
 * view(); ssl::context layers owned callback closures on top. The constructor
 * takes a native SSL_METHOD, keeping this layer free of transport vocabulary.
 */
class BNIO_EXPORT context_base {
 public:
  /**
   * Creates a context from a native method; invalid if SSL_CTX_new fails.
   */
  explicit context_base(const SSL_METHOD* method) noexcept;

  /**
   * Frees the owned SSL_CTX, if any. Does not clear native callback
   * registrations; an owner that installed closures must do that first.
   */
  ~context_base() noexcept;

  /** Copy construction is disabled; the SSL_CTX is uniquely owned. */
  context_base(const context_base&) = delete;

  /** Copy assignment is disabled; the SSL_CTX is uniquely owned. */
  context_base& operator=(const context_base&) = delete;

  /** Moves SSL_CTX ownership from another context. */
  context_base(context_base&& other) noexcept;

  /** Frees the current SSL_CTX, then moves ownership from another context. */
  context_base& operator=(context_base&& other) noexcept;

  /** Returns the owned native SSL_CTX, or nullptr when invalid. */
  [[nodiscard]] SSL_CTX* native_handle() const noexcept { return context_; }

  /** True when a native SSL_CTX is owned. */
  [[nodiscard]] bool valid() const noexcept { return context_ != nullptr; }

  /**
   * Returns a non-owning view of the owned SSL_CTX, invalid when none is
   * owned. The view is valid only while this owner keeps the handle alive.
   */
  [[nodiscard]] context_view view() const noexcept {
    return context_view(context_);
  }

  /** ALPN select callback signature, re-exported from context_view. */
  using alpn_select_cb = context_view::alpn_select_cb;

  /** SNI servername callback signature, re-exported from context_view. */
  using servername_cb = context_view::servername_cb;

  /** client_hello callback signature, re-exported from context_view. */
  using client_hello_cb = context_view::client_hello_cb;

  /** Certificate callback signature, re-exported from context_view. */
  using cert_cb = context_view::cert_cb;

  /** Loads a certificate chain file. */
  [[nodiscard]] std::error_code use_certificate_chain_file(
      const char* path) noexcept {
    return view().use_certificate_chain_file(path);
  }

  /** Loads a PEM private key file. */
  [[nodiscard]] std::error_code use_private_key_file(
      const char* path) noexcept {
    return view().use_private_key_file(path);
  }

  /** Checks the private key against the certificate. */
  [[nodiscard]] std::error_code check_private_key() noexcept {
    return view().check_private_key();
  }

  /** Sets OpenSSL verification flags. */
  void set_verify_mode(int mode) noexcept { view().set_verify_mode(mode); }

  /**
   * Installs a native ALPN callback; arg stays caller-owned and must outlive
   * the native SSL_CTX's use of it.
   */
  void set_alpn_select_cb(alpn_select_cb cb, void* arg) noexcept {
    view().set_alpn_select_cb(cb, arg);
  }

  /**
   * Installs a native SNI servername callback; cb stays caller-owned and must
   * outlive the native SSL_CTX's use of it.
   */
  void set_tlsext_servername_callback(servername_cb cb) noexcept {
    view().set_tlsext_servername_callback(cb);
  }

  /** Sets the SNI callback argument; caller-owned. */
  void set_tlsext_servername_arg(void* arg) noexcept {
    view().set_tlsext_servername_arg(arg);
  }

  /**
   * Installs a native client_hello callback; cb and arg stay caller-owned and
   * must outlive the native SSL_CTX's use of them.
   */
  void set_client_hello_cb(client_hello_cb cb, void* arg) noexcept {
    view().set_client_hello_cb(cb, arg);
  }

  /**
   * Installs a native certificate callback; cb and arg stay caller-owned and
   * must outlive the native SSL_CTX's use of them.
   */
  void set_cert_cb(cert_cb cb, void* arg) noexcept {
    view().set_cert_cb(cb, arg);
  }

  /** Installs an in-memory certificate; cert stays caller-owned. */
  [[nodiscard]] std::error_code use_certificate(X509* cert) noexcept {
    return view().use_certificate(cert);
  }

  /** Installs an in-memory private key; key stays caller-owned. */
  [[nodiscard]] std::error_code use_private_key(EVP_PKEY* key) noexcept {
    return view().use_private_key(key);
  }

  /**
   * Loads one certificate file (SSL_FILETYPE_PEM or ASN1).
   */
  [[nodiscard]] std::error_code use_certificate_file(const char* path,
                                                     int file_type) noexcept {
    return view().use_certificate_file(path, file_type);
  }

  /** Sets the maximum chain verification depth. */
  void set_verify_depth(int depth) noexcept { view().set_verify_depth(depth); }

  /** Adds SSL_OP_* flags to the options already set. */
  void set_options(std::uint64_t options) noexcept {
    view().set_options(options);
  }

  /** Returns the current SSL_OP_* flags. */
  [[nodiscard]] std::uint64_t get_options() const noexcept {
    return view().get_options();
  }

  /** Sets the TLS 1.2 and earlier cipher list. */
  [[nodiscard]] std::error_code set_cipher_list(const char* lists) noexcept {
    return view().set_cipher_list(lists);
  }

  /** Sets the TLS 1.3 cipher suites. */
  [[nodiscard]] std::error_code set_ciphersuites(const char* suites) noexcept {
    return view().set_ciphersuites(suites);
  }

  /**
   * Loads trusted CAs from a file and/or directory; either may be nullptr.
   */
  [[nodiscard]] std::error_code load_verify_locations(
      const char* ca_file, const char* ca_dir) noexcept {
    return view().load_verify_locations(ca_file, ca_dir);
  }

  /** Loads the default trusted CA locations. */
  [[nodiscard]] std::error_code set_default_verify_paths() noexcept {
    return view().set_default_verify_paths();
  }

  /** Sets the minimum protocol version; 0 selects the default. */
  [[nodiscard]] std::error_code set_min_proto_version(int version) noexcept {
    return view().set_min_proto_version(version);
  }

  /** Sets the maximum protocol version; 0 selects the default. */
  [[nodiscard]] std::error_code set_max_proto_version(int version) noexcept {
    return view().set_max_proto_version(version);
  }

  /**
   * Sets the session cache mode; the previous mode is ignored.
   */
  void set_session_cache_mode(long mode) noexcept {
    view().set_session_cache_mode(mode);
  }

  /** Sets the server-side session resumption scope. */
  [[nodiscard]] std::error_code set_session_id_context(
      const unsigned char* sid, std::size_t length) noexcept {
    return view().set_session_id_context(sid, length);
  }

  /**
   * Installs a TLS key material logger for SSLKEYLOGFILE debugging.
   */
  void set_keylog_callback(void (*cb)(const SSL* ssl,
                                      const char* line)) noexcept {
    view().set_keylog_callback(cb);
  }

  /**
   * Sets the client ALPN list in OpenSSL wire format (length-prefixed names).
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
