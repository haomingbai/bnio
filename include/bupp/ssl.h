#pragma once
#ifndef BUPP_SSL_H_
#define BUPP_SSL_H_

#include <bupp/buffer.h>
#include <bupp/io_context.h>
#include <bupp/tcp.h>
#include <openssl/ssl.h>

#include <system_error>
#include <type_traits>
#include <utility>

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

/**
 * RAII owner for an OpenSSL SSL object layered over a next stream.
 *
 * ssl_stream owns the SSL object and the transport halves of its BIO pairs. It
 * also owns or stores the supplied next layer by value. The stream is move-only
 * because SSL ownership is unique.
 */
template <class NextLayer = tcp_socket>
class ssl_stream {
 public:
  /**
   * Creates an SSL stream over a next layer using an existing context.
   */
  ssl_stream(NextLayer next_layer, ssl_context& context) noexcept
      : next_layer_(std::move(next_layer)) {
    reset(context);
  }

  /**
   * Frees the owned SSL object and its BIOs.
   */
  ~ssl_stream() noexcept { release(); }

  /**
   * Copy construction is disabled because the stream owns an SSL object.
   */
  ssl_stream(const ssl_stream&) = delete;

  /**
   * Copy assignment is disabled because the stream owns an SSL object.
   */
  ssl_stream& operator=(const ssl_stream&) = delete;

  /**
   * Moves SSL, BIO, and next-layer ownership from another stream.
   */
  ssl_stream(ssl_stream&& other) noexcept
      : next_layer_(std::move(other.next_layer_)),
        ssl_(std::exchange(other.ssl_, nullptr)),
        read_bio_(std::exchange(other.read_bio_, nullptr)),
        write_bio_(std::exchange(other.write_bio_, nullptr)) {}

  /**
   * Frees the current SSL object and moves ownership from another stream.
   */
  ssl_stream& operator=(ssl_stream&& other) noexcept {
    if (this != &other) {
      release();
      next_layer_ = std::move(other.next_layer_);
      ssl_ = std::exchange(other.ssl_, nullptr);
      read_bio_ = std::exchange(other.read_bio_, nullptr);
      write_bio_ = std::exchange(other.write_bio_, nullptr);
    }
    return *this;
  }

  /**
   * Returns the next layer stored by this SSL stream.
   */
  [[nodiscard]] NextLayer& next_layer() noexcept { return next_layer_; }

  /**
   * Returns the next layer stored by this SSL stream.
   */
  [[nodiscard]] const NextLayer& next_layer() const noexcept {
    return next_layer_;
  }

  /**
   * Returns the lowest layer for asynchronous socket operations.
   */
  [[nodiscard]] decltype(auto) lowest_layer() noexcept {
    return bupp::get_lowest_layer(next_layer_);
  }

  /**
   * Returns the lowest layer for asynchronous socket operations.
   */
  [[nodiscard]] decltype(auto) lowest_layer() const noexcept {
    return bupp::get_lowest_layer(next_layer_);
  }

  /**
   * Returns the owned native SSL pointer, or nullptr when invalid.
   */
  [[nodiscard]] SSL* native_handle() const noexcept { return ssl_; }

  /**
   * Returns the owned native SSL pointer, or nullptr when invalid.
   */
  [[nodiscard]] SSL* get_native_handle() const noexcept {
    return native_handle();
  }

  /**
   * Returns whether this stream owns a native SSL object.
   */
  [[nodiscard]] bool valid() const noexcept { return ssl_ != nullptr; }

  /**
   * Returns the transport BIO used for encrypted input.
   */
  [[nodiscard]] BIO* native_read_bio() const noexcept { return read_bio_; }

  /**
   * Returns the transport BIO used for encrypted output.
   */
  [[nodiscard]] BIO* native_write_bio() const noexcept { return write_bio_; }

  /**
   * Creates a handshake sender whose transport I/O uses the scheduler's queued
   * submission path.
   */
  template <class Scheduler>
  [[nodiscard]] auto async_handshake(Scheduler scheduler,
                                     ssl_handshake_type type);

  /**
   * Creates a handshake sender whose transport I/O is submitted immediately
   * instead of being placed in the scheduler's queued I/O batch.
   */
  template <class Scheduler>
  [[nodiscard]] auto async_handshake_direct(Scheduler scheduler,
                                            ssl_handshake_type type);

  /**
   * Creates a sender for one plaintext SSL read operation. The operation may
   * complete with fewer bytes than the buffer size.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_read(Scheduler scheduler, Buffer&& buffer,
                                int flags = 0);

  /**
   * Creates a sender for one plaintext SSL read operation. This is the explicit
   * read-some spelling of async_read.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_read_some(Scheduler scheduler, Buffer&& buffer,
                                     int flags = 0);

  /**
   * Creates a read sender whose transport I/O is submitted immediately instead
   * of being placed in the scheduler's queued I/O batch.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_read_direct(Scheduler scheduler, Buffer&& buffer,
                                       int flags = 0);

  /**
   * Creates a sender for one direct-submission plaintext SSL read operation.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_read_some_direct(Scheduler scheduler,
                                            Buffer&& buffer, int flags = 0);

  /**
   * Creates a sender that writes the whole plaintext buffer through SSL.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_write(Scheduler scheduler, Buffer&& buffer,
                                 int flags = 0);

  /**
   * Creates a sender for one plaintext SSL write step without retrying short
   * SSL_write results.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_write_some(Scheduler scheduler, Buffer&& buffer,
                                      int flags = 0);

  /**
   * Creates a write sender that writes the whole plaintext buffer and submits
   * each lower-level transport I/O immediately instead of placing it in the
   * scheduler's queued I/O batch.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_write_direct(Scheduler scheduler, Buffer&& buffer,
                                        int flags = 0);

  /**
   * Creates a sender for one direct-submission plaintext SSL write step without
   * retrying short SSL_write results.
   */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_write_some_direct(Scheduler scheduler,
                                             Buffer&& buffer, int flags = 0);

  /**
   * Creates a shutdown sender whose transport I/O uses the scheduler's queued
   * submission path.
   */
  template <class Scheduler>
  [[nodiscard]] auto async_shutdown(Scheduler scheduler);

  /**
   * Creates a shutdown sender whose transport I/O is submitted immediately
   * instead of being placed in the scheduler's queued I/O batch.
   */
  template <class Scheduler>
  [[nodiscard]] auto async_shutdown_direct(Scheduler scheduler);

 private:
  void release() noexcept {
    if (ssl_ != nullptr) {
      SSL_free(ssl_);
      ssl_ = nullptr;
    }
    if (read_bio_ != nullptr) {
      BIO_free(read_bio_);
      read_bio_ = nullptr;
    }
    if (write_bio_ != nullptr) {
      BIO_free(write_bio_);
      write_bio_ = nullptr;
    }
  }

  void reset(ssl_context& context) noexcept {
    ssl_ = SSL_new(context.native_handle());
    if (ssl_ == nullptr) {
      return;
    }

    constexpr std::size_t bio_buffer_size = 64 * 1024;
    BIO* ssl_read_bio = nullptr;
    BIO* ssl_write_bio = nullptr;
    if (BIO_new_bio_pair(&ssl_read_bio, 0, &read_bio_, bio_buffer_size) != 1 ||
        BIO_new_bio_pair(&ssl_write_bio, bio_buffer_size, &write_bio_, 0) !=
            1) {
      BIO_free(ssl_read_bio);
      BIO_free(ssl_write_bio);
      BIO_free(read_bio_);
      BIO_free(write_bio_);
      SSL_free(ssl_);
      ssl_ = nullptr;
      read_bio_ = nullptr;
      write_bio_ = nullptr;
      return;
    }

    SSL_set_bio(ssl_, ssl_read_bio, ssl_write_bio);
    SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE |
                           SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  }

  NextLayer next_layer_;
  SSL* ssl_ = nullptr;
  BIO* read_bio_ = nullptr;
  BIO* write_bio_ = nullptr;
};

}  // namespace bupp

#include <bupp/detail/ssl/async_operations.h>

namespace bupp {

template <class NextLayer>
template <class Scheduler>
auto ssl_stream<NextLayer>::async_handshake(Scheduler scheduler,
                                            ssl_handshake_type type) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::ssl_handshake_sender<scheduler_type, NextLayer, false>(
      std::move(scheduler), *this, type);
}

template <class NextLayer>
template <class Scheduler>
auto ssl_stream<NextLayer>::async_handshake_direct(Scheduler scheduler,
                                                   ssl_handshake_type type) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::ssl_handshake_sender<scheduler_type, NextLayer, true>(
      std::move(scheduler), *this, type);
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_read(Scheduler scheduler, Buffer&& buffer,
                                       int) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_read_sender<scheduler_type, NextLayer, holder_type, false>(
      std::move(scheduler), *this, std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_read_some(Scheduler scheduler,
                                            Buffer&& buffer, int) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_read_sender<scheduler_type, NextLayer, holder_type, false>(
      std::move(scheduler), *this, std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_read_direct(Scheduler scheduler,
                                              Buffer&& buffer, int) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_read_sender<scheduler_type, NextLayer, holder_type, true>(
      std::move(scheduler), *this, std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_read_some_direct(Scheduler scheduler,
                                                   Buffer&& buffer, int) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_read_sender<scheduler_type, NextLayer, holder_type, true>(
      std::move(scheduler), *this, std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_write(Scheduler scheduler, Buffer&& buffer,
                                        int) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_write_sender<scheduler_type, NextLayer, holder_type, false,
                                  true>(std::move(scheduler), *this,
                                        std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_write_some(Scheduler scheduler,
                                             Buffer&& buffer, int) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_write_sender<scheduler_type, NextLayer, holder_type, false,
                                  false>(std::move(scheduler), *this,
                                         std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_write_direct(Scheduler scheduler,
                                               Buffer&& buffer, int) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_write_sender<scheduler_type, NextLayer, holder_type, true,
                                  true>(std::move(scheduler), *this,
                                        std::move(holder));
}

template <class NextLayer>
template <class Scheduler, class Buffer>
auto ssl_stream<NextLayer>::async_write_some_direct(Scheduler scheduler,
                                                    Buffer&& buffer, int) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using holder_type = decltype(holder);
  return detail::ssl_write_sender<scheduler_type, NextLayer, holder_type, true,
                                  false>(std::move(scheduler), *this,
                                         std::move(holder));
}

template <class NextLayer>
template <class Scheduler>
auto ssl_stream<NextLayer>::async_shutdown(Scheduler scheduler) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::ssl_shutdown_sender<scheduler_type, NextLayer, false>(
      std::move(scheduler), *this);
}

template <class NextLayer>
template <class Scheduler>
auto ssl_stream<NextLayer>::async_shutdown_direct(Scheduler scheduler) {
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  return detail::ssl_shutdown_sender<scheduler_type, NextLayer, true>(
      std::move(scheduler), *this);
}

/**
 * Customization point object for Provider::async_handshake.
 */
struct async_handshake_t {
  /**
   * Invokes async_handshake on a stream.
   */
  template <class Provider, class Stream>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      ssl_handshake_type type) const {
    return std::forward<Stream>(stream).async_handshake(
        std::forward<Provider>(provider), type);
  }
};

/**
 * Customization point object for direct-submission SSL handshake.
 */
struct async_handshake_direct_t {
  /**
   * Invokes async_handshake_direct on a stream.
   */
  template <class Provider, class Stream>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      ssl_handshake_type type) const {
    return std::forward<Stream>(stream).async_handshake_direct(
        std::forward<Provider>(provider), type);
  }
};

/**
 * Customization point object for Provider::async_shutdown.
 */
struct async_shutdown_t {
  /**
   * Invokes async_shutdown on a stream.
   */
  template <class Provider, class Stream>
  constexpr decltype(auto) operator()(Provider&& provider,
                                      Stream&& stream) const {
    return std::forward<Stream>(stream).async_shutdown(
        std::forward<Provider>(provider));
  }
};

/**
 * Customization point object for direct-submission SSL shutdown.
 */
struct async_shutdown_direct_t {
  /**
   * Invokes async_shutdown_direct on a stream.
   */
  template <class Provider, class Stream>
  constexpr decltype(auto) operator()(Provider&& provider,
                                      Stream&& stream) const {
    return std::forward<Stream>(stream).async_shutdown_direct(
        std::forward<Provider>(provider));
  }
};

/**
 * Customization point object instance for async_handshake.
 */
inline constexpr async_handshake_t async_handshake{};

/**
 * Customization point object instance for async_handshake_direct.
 */
inline constexpr async_handshake_direct_t async_handshake_direct{};

/**
 * Customization point object instance for async_shutdown.
 */
inline constexpr async_shutdown_t async_shutdown{};

/**
 * Customization point object instance for async_shutdown_direct.
 */
inline constexpr async_shutdown_direct_t async_shutdown_direct{};

}  // namespace bupp

#endif  // BUPP_SSL_H_
