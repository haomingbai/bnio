/**
 * @file ssl.cpp
 * @brief SSL base layer: OpenSSL error category, context_base owner, and
 * context_view operations.
 */

#include <bnio/ssl/base.h>
#include <bnio/ssl/callback.h>
#include <bnio/ssl/handshake_pause.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <array>
#include <string>
#include <system_error>
#include <utility>

namespace bnio {

namespace {

// Value reported by make_no_ssl_error(). Real OpenSSL codes are non-negative
// (ERR_get_error() returns 0 only for an empty queue), so -1 cannot collide.
constexpr int k_no_ssl_error_value = -1;

class openssl_category final : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override { return "openssl"; }

  [[nodiscard]] std::string message(int value) const override {
    if (value == k_no_ssl_error_value) {
      return "no OpenSSL error was recorded";
    }
    std::array<char, 256> buffer{};
    ERR_error_string_n(static_cast<unsigned long>(value), buffer.data(),
                       buffer.size());
    return buffer.data();
  }
};

class handshake_pause_category_impl final : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override {
    return "bnio.ssl.handshake_pause";
  }

  [[nodiscard]] std::string message(int value) const override {
    switch (static_cast<ssl::handshake_pause_reason>(value)) {
      case ssl::handshake_pause_reason::none:
        return "no handshake pause";
      case ssl::handshake_pause_reason::want_read:
        return "handshake paused waiting for readable transport data";
      case ssl::handshake_pause_reason::want_write:
        return "handshake paused waiting for writable transport data";
      case ssl::handshake_pause_reason::x509_lookup:
        return "handshake paused for an X509 lookup";
      case ssl::handshake_pause_reason::async:
        return "handshake paused for an asynchronous engine operation";
      case ssl::handshake_pause_reason::async_job:
        return "handshake paused for an asynchronous job to be assigned";
      case ssl::handshake_pause_reason::client_hello_cb:
        return "handshake paused in the client hello callback";
      case ssl::handshake_pause_reason::retry_verify:
        return "handshake paused for certificate verification retry";
    }
    return "unknown handshake pause reason";
  }
};

}  // namespace

namespace ssl {

const std::error_category& handshake_pause_category() noexcept {
  // A function-local static keeps the category a single object for every
  // translation unit and shared object that links against the bnio library.
  static handshake_pause_category_impl category;
  return category;
}

std::error_code make_handshake_pause_error(
    handshake_pause_reason reason) noexcept {
  const int value = static_cast<int>(reason);
  if (value == static_cast<int>(handshake_pause_reason::none)) {
    return std::error_code{};
  }
  return std::error_code(value, handshake_pause_category());
}

alert_out::alert_out(int* alert) noexcept : alert_(alert) {}

void alert_out::set(int alert) noexcept {
  if (alert_ != nullptr) {
    *alert_ = alert;
  }
}

sni_out::sni_out(SSL* ssl, int* alert) noexcept : ssl_(ssl), alert_(alert) {}

std::error_code sni_out::set_context(context_view ctx) noexcept {
  if (ssl_ == nullptr || !ctx.valid()) {
    return base::make_no_ssl_error();
  }
  base::clear_errors();
  // SSL_set_SSL_CTX takes its own ref; the bnio owner may die after the switch.
  if (SSL_set_SSL_CTX(ssl_, ctx.native_handle()) == nullptr) {
    return base::last_error();
  }
  return base::empty_error_code;
}

void sni_out::set_alert(int alert) noexcept {
  if (alert_ != nullptr) {
    *alert_ = alert;
  }
}

cert_out::cert_out(SSL* ssl) noexcept : ssl_(ssl) {}

std::error_code cert_out::use_certificate(X509* cert) noexcept {
  if (ssl_ == nullptr) {
    return base::make_no_ssl_error();
  }
  base::clear_errors();
  // OpenSSL keeps its own reference; caller retains ownership.
  if (SSL_use_certificate(ssl_, cert) == 1) {
    return base::empty_error_code;
  }
  return base::last_error();
}

std::error_code cert_out::use_private_key(EVP_PKEY* key) noexcept {
  if (ssl_ == nullptr) {
    return base::make_no_ssl_error();
  }
  base::clear_errors();
  // OpenSSL keeps its own reference; caller retains ownership.
  if (SSL_use_PrivateKey(ssl_, key) == 1) {
    return base::empty_error_code;
  }
  return base::last_error();
}

}  // namespace ssl

namespace ssl::base {

const std::error_category& openssl_error_category() noexcept {
  static openssl_category category;
  return category;
}

std::error_code make_openssl_error(unsigned long code) noexcept {
  return std::error_code(static_cast<int>(code), openssl_error_category());
}

std::error_code make_no_ssl_error() noexcept {
  return std::error_code(k_no_ssl_error_value, openssl_error_category());
}

context_base::context_base(const SSL_METHOD* method) noexcept
    : context_(SSL_CTX_new(method)) {}

context_base::~context_base() noexcept {
  if (context_ != nullptr) {
    SSL_CTX_free(context_);
  }
}

context_base::context_base(context_base&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)) {}

context_base& context_base::operator=(context_base&& other) noexcept {
  if (this != &other) {
    if (context_ != nullptr) {
      SSL_CTX_free(context_);
    }
    context_ = std::exchange(other.context_, nullptr);
  }
  return *this;
}

std::error_code context_view::use_certificate_chain_file(
    const char* path) noexcept {
  clear_errors();
  if (SSL_CTX_use_certificate_chain_file(context_, path) == 1) {
    return empty_error_code;
  }
  return last_error();
}

std::error_code context_view::use_private_key_file(const char* path) noexcept {
  clear_errors();
  if (SSL_CTX_use_PrivateKey_file(context_, path, SSL_FILETYPE_PEM) == 1) {
    return empty_error_code;
  }
  return last_error();
}

std::error_code context_view::check_private_key() noexcept {
  clear_errors();
  if (SSL_CTX_check_private_key(context_) == 1) {
    return empty_error_code;
  }
  return last_error();
}

void context_view::set_verify_mode(int mode) noexcept {
  SSL_CTX_set_verify(context_, mode, nullptr);
}

void context_view::set_alpn_select_cb(alpn_select_cb cb, void* arg) noexcept {
  SSL_CTX_set_alpn_select_cb(context_, cb, arg);
}

void context_view::set_tlsext_servername_callback(servername_cb cb) noexcept {
  SSL_CTX_set_tlsext_servername_callback(context_, cb);
}

void context_view::set_tlsext_servername_arg(void* arg) noexcept {
  SSL_CTX_set_tlsext_servername_arg(context_, arg);
}

void context_view::set_client_hello_cb(client_hello_cb cb, void* arg) noexcept {
  SSL_CTX_set_client_hello_cb(context_, cb, arg);
}

void context_view::set_cert_cb(cert_cb cb, void* arg) noexcept {
  SSL_CTX_set_cert_cb(context_, cb, arg);
}

std::error_code context_view::use_certificate(X509* cert) noexcept {
  clear_errors();
  // OpenSSL keeps its own reference; caller retains ownership.
  if (SSL_CTX_use_certificate(context_, cert) == 1) {
    return empty_error_code;
  }
  return last_error();
}

std::error_code context_view::use_private_key(EVP_PKEY* key) noexcept {
  clear_errors();
  // OpenSSL keeps its own reference; caller retains ownership.
  if (SSL_CTX_use_PrivateKey(context_, key) == 1) {
    return empty_error_code;
  }
  return last_error();
}

}  // namespace ssl::base

}  // namespace bnio
