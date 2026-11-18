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
 * Enumerator values match the SSL_CLIENT_HELLO_* constants, so the framework
 * trampoline converts with a plain static_cast.
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
   * Pause the handshake and retry the callback later through the
   * factory-driven retry surface (SSL_CLIENT_HELLO_RETRY).
   */
  retry = -1,
};

/**
 * Result a certificate callback reports to OpenSSL.
 *
 * Enumerator values match the values OpenSSL expects from a certificate
 * callback, so the framework trampoline converts with a plain static_cast.
 */
enum class cert_result : int {
  /**
   * Abort the handshake (callback return value 0).
   */
  fail = 0,

  /**
   * Continue with the certificate and key installed through cert_out
   * (callback return value 1).
   */
  ok = 1,

  /**
   * Pause the handshake with SSL_ERROR_WANT_X509_LOOKUP and retry later
   * (callback return value -1).
   */
  retry = -1,
};

/**
 * Write-back alert code for a callback abort; user callbacks never construct
 * one, writes to a null slot are ignored, and setting it does not send the
 * alert unless the callback also returns a failing result.
 */
class BNIO_EXPORT alert_out {
 public:
  /**
   * Internal: wraps OpenSSL's alert out-parameter. User callbacks receive an
   * alert_out by reference and never construct one.
   */
  explicit alert_out(int* alert) noexcept;

  /**
   * Records the OpenSSL alert code (e.g. SSL_AD_INTERNAL_ERROR) sent when the
   * callback returns a fatal or error result; a null alert pointer ignores it.
   */
  void set(int alert) noexcept;

 private:
  int* alert_;
};

/**
 * Callback proxy for SNI servername callbacks.
 *
 * The framework trampoline passes it by reference; user callbacks never
 * construct one. It exposes the two write operations OpenSSL's servername
 * callback supports: switching the SSL_CTX driving the connection and
 * recording an alert code for a failed handshake.
 */
class BNIO_EXPORT sni_out {
 public:
  /**
   * Internal: wraps the connection and OpenSSL's alert out-parameter. User
   * callbacks receive an sni_out by reference and never construct one.
   */
  sni_out(SSL* ssl, int* alert) noexcept;

  /**
   * SSL_set_SSL_CTX for this connection. If the target context has a
   * certificate callback, that context and its closure must outlive this
   * stream (SSL_set_SSL_CTX copies cert_cb_arg); ALPN/SNI/client_hello
   * registrations are not copied and are safe to clear. Returns the OpenSSL
   * error, or make_no_ssl_error() for null inputs.
   */
  [[nodiscard]] std::error_code set_context(context_view ctx) noexcept;

  /**
   * Records the OpenSSL alert code (e.g. SSL_AD_INTERNAL_ERROR) sent when the
   * callback returns a fatal result; a null alert pointer ignores it.
   */
  void set_alert(int alert) noexcept;

 private:
  SSL* ssl_;
  int* alert_;
};

/**
 * Callback proxy for per-connection certificate selection; the framework
 * trampoline passes it by reference and user callbacks never construct one.
 * Certificates and keys installed through it apply to the connection only,
 * not to the shared SSL_CTX.
 */
class BNIO_EXPORT cert_out {
 public:
  /**
   * Internal: wraps the connection being handshaken. User callbacks receive a
   * cert_out by reference and never construct one.
   */
  explicit cert_out(SSL* ssl) noexcept;

  /**
   * Installs a certificate on this connection, mirroring
   * SSL_use_certificate. OpenSSL increments the refcount; the caller keeps
   * ownership of cert. Failure returns the recorded OpenSSL error; a null
   * connection reports base::make_no_ssl_error().
   */
  [[nodiscard]] std::error_code use_certificate(X509* cert) noexcept;

  /**
   * Installs a private key on this connection, mirroring SSL_use_PrivateKey.
   * OpenSSL increments the refcount; the caller keeps ownership of key.
   * Failure returns the recorded OpenSSL error; a null connection reports
   * base::make_no_ssl_error().
   */
  [[nodiscard]] std::error_code use_private_key(EVP_PKEY* key) noexcept;

 private:
  SSL* ssl_;
};

}  // namespace bnio::ssl

#endif  // BNIO_SSL_CALLBACK_H_
