#pragma once
#ifndef BUPP_SSL_CONTEXT_H_
#define BUPP_SSL_CONTEXT_H_

#include <bupp/export.h>
#include <openssl/ssl.h>

#include <system_error>

namespace bupp {

/**
 * OpenSSL SSL_CTX method selector.
 */
enum class ssl_context_method {
  /**
   * Generic TLS method.
   */
  tls,

  /**
   * TLS client method.
   */
  tls_client,

  /**
   * TLS server method.
   */
  tls_server,
};

/**
 * Direction used when starting an SSL/TLS handshake.
 */
enum class ssl_handshake_type {
  /**
   * Start a client-side handshake.
   */
  client,

  /**
   * Start a server-side handshake.
   */
  server,
};

/**
 * Returns the error category used for OpenSSL library errors.
 */
[[nodiscard]] BUPP_EXPORT const std::error_category&
openssl_error_category() noexcept;

/**
 * Creates an error_code in the OpenSSL error category.
 */
[[nodiscard]] BUPP_EXPORT std::error_code make_openssl_error(
    unsigned long code) noexcept;

/**
 * RAII owner for an OpenSSL SSL_CTX object.
 *
 * ssl_context owns the native context and frees it on destruction. It is
 * move-only because the native SSL_CTX ownership is unique.
 */
class BUPP_EXPORT ssl_context {
 public:
  /**
   * Creates an SSL context for the selected TLS method.
   */
  explicit ssl_context(
      ssl_context_method method = ssl_context_method::tls) noexcept;

  /**
   * Frees the owned SSL_CTX, if any.
   */
  ~ssl_context() noexcept;

  /**
   * Copy construction is disabled because the context owns an SSL_CTX.
   */
  ssl_context(const ssl_context&) = delete;

  /**
   * Copy assignment is disabled because the context owns an SSL_CTX.
   */
  ssl_context& operator=(const ssl_context&) = delete;

  /**
   * Moves SSL_CTX ownership from another context.
   */
  ssl_context(ssl_context&& other) noexcept;

  /**
   * Frees the current SSL_CTX and moves ownership from another context.
   */
  ssl_context& operator=(ssl_context&& other) noexcept;

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

 private:
  SSL_CTX* context_ = nullptr;
};

}  // namespace bupp

#endif  // BUPP_SSL_CONTEXT_H_
