#include <bupp/ssl.h>
#include <openssl/err.h>

#include <array>
#include <utility>

namespace bupp {

namespace {

class openssl_category final : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override { return "openssl"; }

  [[nodiscard]] std::string message(int value) const override {
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

[[nodiscard]] std::error_code last_context_error() noexcept {
  const unsigned long error = ERR_get_error();
  if (error == 0) {
    return std::make_error_code(std::errc::protocol_error);
  }
  return make_openssl_error(error);
}

}  // namespace

const std::error_category& openssl_error_category() noexcept {
  static openssl_category category;
  return category;
}

std::error_code make_openssl_error(unsigned long code) noexcept {
  return std::error_code(static_cast<int>(code), openssl_error_category());
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
  if (SSL_CTX_use_certificate_chain_file(context_, path) == 1) {
    return {};
  }
  return last_context_error();
}

std::error_code ssl_context::use_private_key_file(const char* path) noexcept {
  if (SSL_CTX_use_PrivateKey_file(context_, path, SSL_FILETYPE_PEM) == 1) {
    return {};
  }
  return last_context_error();
}

std::error_code ssl_context::check_private_key() noexcept {
  if (SSL_CTX_check_private_key(context_) == 1) {
    return {};
  }
  return last_context_error();
}

void ssl_context::set_verify_mode(int mode) noexcept {
  SSL_CTX_set_verify(context_, mode, nullptr);
}

}  // namespace bupp
