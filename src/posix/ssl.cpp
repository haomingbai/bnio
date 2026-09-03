/**
 * @file ssl.cpp
 * @brief SSL context RAII owner and OpenSSL error category implementation.
 */

#include <bnio/detail/ssl/async_operations/common.h>
#include <bnio/ssl.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <array>
#include <string>
#include <system_error>
#include <utility>

#include "bnio/ssl/context.h"
namespace bnio {

namespace {

// The value make_no_ssl_error() reports. Real OpenSSL error codes are
// non-negative (ERR_get_error() returns 0 only for an empty queue), so -1
// can never collide with a genuine error code.
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

[[nodiscard]] const SSL_METHOD* select_method(
    ssl_context_method method) noexcept {
  switch (method) {
    case ssl_context_method::tls_client:
      return TLS_client_method();
    case ssl_context_method::tls_server:
      return TLS_server_method();
    case ssl_context_method::tls:
      return TLS_method();
  }
  return TLS_method();
}

}  // namespace

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

ssl_context::ssl_context(ssl_context_method method) noexcept
    : context_(SSL_CTX_new(select_method(method))) {}

ssl_context::~ssl_context() noexcept {
  if (context_ != nullptr) {
    SSL_CTX_free(context_);
  }
}

ssl_context::ssl_context(ssl_context&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)) {}

ssl_context& ssl_context::operator=(ssl_context&& other) noexcept {
  if (this != &other) {
    if (context_ != nullptr) {
      SSL_CTX_free(context_);
    }
    context_ = std::exchange(other.context_, nullptr);
  }
  return *this;
}

std::error_code ssl_context::use_certificate_chain_file(
    const char* path) noexcept {
  detail::clear_ssl_errors();
  if (SSL_CTX_use_certificate_chain_file(context_, path) == 1) {
    return {};
  }
  return detail::last_ssl_error();
}

std::error_code ssl_context::use_private_key_file(const char* path) noexcept {
  detail::clear_ssl_errors();
  if (SSL_CTX_use_PrivateKey_file(context_, path, SSL_FILETYPE_PEM) == 1) {
    return {};
  }
  return detail::last_ssl_error();
}

std::error_code ssl_context::check_private_key() noexcept {
  detail::clear_ssl_errors();
  if (SSL_CTX_check_private_key(context_) == 1) {
    return {};
  }
  return detail::last_ssl_error();
}

void ssl_context::set_verify_mode(int mode) noexcept {
  SSL_CTX_set_verify(context_, mode, nullptr);
}

}  // namespace bnio
