/**
 * @file ssl.cpp
 * @brief SSL base layer: OpenSSL error category and context_base implementation.
 */

#include <bnio/ssl/base.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <array>
#include <string>
#include <system_error>
#include <utility>

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

}  // namespace

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
  store_owned_alpn_arg(nullptr);
  if (context_ != nullptr) {
    SSL_CTX_free(context_);
  }
}

context_base::context_base(context_base&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)),
      owned_alpn_arg_(std::exchange(other.owned_alpn_arg_, nullptr)) {}

context_base& context_base::operator=(context_base&& other) noexcept {
  if (this != &other) {
    if (owned_alpn_arg_ != nullptr) {
      owned_alpn_arg_->destroy();
    }
    if (context_ != nullptr) {
      SSL_CTX_free(context_);
    }
    context_ = std::exchange(other.context_, nullptr);
    owned_alpn_arg_ = std::exchange(other.owned_alpn_arg_, nullptr);
  }
  return *this;
}

std::error_code context_base::use_certificate_chain_file(
    const char* path) noexcept {
  clear_errors();
  if (SSL_CTX_use_certificate_chain_file(context_, path) == 1) {
    return empty_error_code;
  }
  return last_error();
}

std::error_code context_base::use_private_key_file(const char* path) noexcept {
  clear_errors();
  if (SSL_CTX_use_PrivateKey_file(context_, path, SSL_FILETYPE_PEM) == 1) {
    return empty_error_code;
  }
  return last_error();
}

std::error_code context_base::check_private_key() noexcept {
  clear_errors();
  if (SSL_CTX_check_private_key(context_) == 1) {
    return empty_error_code;
  }
  return last_error();
}

void context_base::set_verify_mode(int mode) noexcept {
  SSL_CTX_set_verify(context_, mode, nullptr);
}

void context_base::set_alpn_select_cb(alpn_select_cb cb, void* arg) noexcept {
  SSL_CTX_set_alpn_select_cb(context_, cb, arg);
}

void context_base::store_owned_alpn_arg(alpn_arg_base* owned_arg) noexcept {
  if (owned_alpn_arg_ != nullptr) {
    owned_alpn_arg_->destroy();
  }
  owned_alpn_arg_ = owned_arg;
}

}  // namespace ssl::base

}  // namespace bnio
