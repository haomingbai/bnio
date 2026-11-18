/**
 * @file callback.h
 * @brief Callback result enumerators and framework-provided callback proxies.
 */

#pragma once
#ifndef BNIO_SSL_CALLBACK_H_
#define BNIO_SSL_CALLBACK_H_

#include <bnio/export.h>
#include <bnio/ssl/context_view.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <system_error>

namespace bnio::ssl {

/**
 * Result a client_hello callback reports to OpenSSL.
 *
 * The enumerator values match the SSL_CLIENT_HELLO_* constants, so the
 * framework trampoline converts a result to the int OpenSSL consumes with a
 * plain static_cast and needs no value-mapping branch.
 */
enum class client_hello_result : int {
  /**
   * Abort the handshake with the alert recorded through alert_out, if any
   * (SSL_CLIENT_HELLO_ERROR).
   */
  error = 0,

  /**
   * Continue the handshake (SSL_CLIENT_HELLO_SUCCESS).
   */
  success = 1,

  /**
   * Pause the handshake and retry the callback later, driven by the
   * handshake retry surface (SSL_CLIENT_HELLO_RETRY).
   */
  retry = -1,
};

/**
 * Result a certificate callback reports to OpenSSL.
 *
 * The enumerator values match the values OpenSSL expects from a certificate
 * callback, so the framework trampoline converts a result with a plain
 * static_cast and needs no value-mapping branch.
 */
enum class cert_result : int {
  /**
   * Abort the handshake (callback return value 0).
   */
  fail = 0,

  /**
   * Continue the handshake with the certificate and key installed through
   * cert_out (callback return value 1).
   */
  ok = 1,

  /**
   * Pause the handshake with SSL_ERROR_WANT_X509_LOOKUP and retry the
   * callback later (callback return value -1).
   */
  retry = -1,
};

/**
 * Write-back proxy for the OpenSSL alert code a callback wants sent when it
 * aborts the handshake.
 *
 * The framework trampoline constructs an alert_out and passes it to a
 * client_hello callback by reference; user callbacks never construct one.
 * Setting an alert only records the code: the callback still has to return a
 * fatal or error result for OpenSSL to send it.
 *
 * Writing to a proxy built over a null alert pointer is a no-op, because some
 * OpenSSL call sites pass no alert slot. The null branch is reachable through
 * public construction and is covered by unit tests.
 */
class BNIO_EXPORT alert_out {
 public:
  /**
   * Internal: wraps OpenSSL's alert out-parameter for the framework
   * trampoline. User callbacks receive an alert_out by reference and never
   * construct one.
   */
  explicit alert_out(int* alert) noexcept;

  /**
   * Records the OpenSSL alert code (for example SSL_AD_INTERNAL_ERROR) that
   * OpenSSL sends when the callback returns a fatal or error result. Writes
   * nothing when the wrapped alert pointer is null.
   */
  void set(int alert) noexcept;

 private:
  int* alert_;
};

/**
 * Callback proxy for SNI servername callbacks.
 *
 * The framework trampoline constructs an sni_out and passes it to a
 * servername callback by reference; user callbacks never construct one. It
 * exposes the two write operations OpenSSL's servername callback supports:
 * switching the SSL_CTX that drives the connection and recording an alert
 * code for a failed handshake.
 */
class BNIO_EXPORT sni_out {
 public:
  /**
   * Internal: wraps the connection being handshaken and OpenSSL's alert
   * out-parameter for the servername callback trampoline. User callbacks
   * receive an sni_out by reference and never construct one.
   */
  sni_out(SSL* ssl, int* alert) noexcept;

  /**
   * Switches the SSL_CTX driving this connection, mirroring SSL_set_SSL_CTX.
   *
   * The new context must be valid. OpenSSL duplicates the target SSL_CTX's
   * certificate callback registration into the connection at switch time, so
   * if the target context has a certificate callback installed, the target
   * context (and its closure) must outlive this stream. Switching to a
   * context that is destroyed earlier is unsupported and leaves the
   * connection holding a copied callback argument that refers to freed
   * state. Callbacks that OpenSSL looks up dynamically through the current
   * SSL_CTX (ALPN, servername, client_hello) are not copied and remain safe
   * to clear on destruction.
   *
   * Returns an empty error_code on success and the OpenSSL error recorded by
   * the failed call otherwise (base::clear_errors, base::last_error). A null
   * connection or a null context is reported as base::make_no_ssl_error().
   */
  [[nodiscard]] std::error_code set_context(context_view ctx) noexcept;

  /**
   * Records the OpenSSL alert code (for example SSL_AD_INTERNAL_ERROR) that
   * OpenSSL sends when the callback returns a fatal result. Writes nothing
   * when the wrapped alert pointer is null.
   */
  void set_alert(int alert) noexcept;

 private:
  SSL* ssl_;
  int* alert_;
};

/**
 * Callback proxy for per-connection certificate selection.
 *
 * The framework trampoline constructs a cert_out and passes it to a
 * certificate callback by reference; user callbacks never construct one.
 * Certificates and keys installed through this proxy apply to the connection
 * only, not to the shared SSL_CTX.
 */
class BNIO_EXPORT cert_out {
 public:
  /**
   * Internal: wraps the connection being handshaken for the certificate
   * callback trampoline. User callbacks receive a cert_out by reference and
   * never construct one.
   */
  explicit cert_out(SSL* ssl) noexcept;

  /**
   * Installs a certificate on this connection, mirroring SSL_use_certificate.
   *
   * OpenSSL increments the certificate reference count; the caller keeps
   * ownership of cert. Returns an empty error_code on success and the OpenSSL
   * error recorded by the failed call otherwise (base::clear_errors,
   * base::last_error). A null connection is reported as
   * base::make_no_ssl_error().
   */
  [[nodiscard]] std::error_code use_certificate(X509* cert) noexcept;

  /**
   * Installs a private key on this connection, mirroring
   * SSL_use_PrivateKey.
   *
   * OpenSSL increments the key reference count; the caller keeps ownership of
   * key. Returns an empty error_code on success and the OpenSSL error recorded
   * by the failed call otherwise (base::clear_errors, base::last_error). A
   * null connection is reported as base::make_no_ssl_error().
   */
  [[nodiscard]] std::error_code use_private_key(EVP_PKEY* key) noexcept;

 private:
  SSL* ssl_;
};

}  // namespace bnio::ssl

#endif  // BNIO_SSL_CALLBACK_H_
