/**
 * @file stream_class.h
 * @brief SSL stream class template definition.
 */

#pragma once
#ifndef BNIO_SSL_TCP_STREAM_CLASS_H_
#define BNIO_SSL_TCP_STREAM_CLASS_H_

#include <bnio/io_context.h>
#include <bnio/ssl/base/bio_pair.h>
#include <bnio/ssl/base/context_base.h>
#include <bnio/ssl/base/context_view.h>
#include <bnio/ssl/base/errors.h>
#include <bnio/ssl/context.h>
#include <bnio/ssl/context_view.h>
#include <bnio/ssl/detail/handshake_concepts.h>
#include <bnio/ssl/stream_view.h>
#include <bnio/tcp.h>
#include <openssl/ssl.h>

#include <span>
#include <system_error>
#include <utility>

namespace bnio::ssl::tcp {

/**
 * RAII owner for an OpenSSL SSL object layered over a next stream.
 *
 * stream owns the SSL object and the transport halves of its BIO pair. It also
 * owns or stores the supplied next layer by value. The stream is move-only
 * because SSL ownership is unique.
 */
template <class NextLayer = bnio::tcp_socket>
class stream {
 public:
  /**
   * Creates an SSL stream over a next layer using an owning context.
   *
   * This is the primary constructor. The stream takes no ownership of the
   * context and keeps no bnio reference to it: it only needs the native
   * SSL_CTX, and SSL_new takes its own OpenSSL reference, so the stream may
   * safely outlive the bnio context. The context only has to outlive every
   * stream whose handshake should still invoke its ALPN/keylog callbacks.
   * Destroying the context clears those native registrations and the closures
   * even while the connection itself stays alive.
   */
  stream(NextLayer next_layer, bnio::ssl::context& context) noexcept
      : next_layer_(std::move(next_layer)) {
    reset(context);
  }

  /**
   * Creates an SSL stream over a next layer using an owning base context.
   * Equivalent to the ssl::context overload: only the native SSL_CTX is
   * needed, and SSL_new takes its own OpenSSL reference to it.
   */
  stream(NextLayer next_layer, base::context_base& context) noexcept
      : next_layer_(std::move(next_layer)) {
    reset(context);
  }

  /**
   * Creates an SSL stream from a non-owning context view.
   *
   * Use this only when the caller already holds a non-owning handle; passing
   * the owning ssl::context is the primary entry point. The stream takes no
   * ownership of the context. SSL_new keeps the native SSL_CTX alive through
   * the SSL object's own reference, so the stream may safely outlive the bnio
   * context; the context only has to outlive every stream whose handshake
   * should still invoke its ALPN/keylog callbacks. Destroying the context
   * clears those native registrations and the closures even while the
   * connection itself stays alive.
   */
  stream(NextLayer next_layer, context_view context) noexcept
      : next_layer_(std::move(next_layer)) {
    reset(context);
  }

  /**
   * Frees the owned SSL object and its BIOs.
   */
  ~stream() noexcept { release(); }

  /**
   * Copy construction is disabled because the stream owns an SSL object.
   */
  stream(const stream&) = delete;

  /**
   * Copy assignment is disabled because the stream owns an SSL object.
   */
  stream& operator=(const stream&) = delete;

  /**
   * Moves SSL, BIO, and next-layer ownership from another stream.
   */
  stream(stream&& other) noexcept
      : next_layer_(std::move(other.next_layer_)),
        ssl_(std::exchange(other.ssl_, stream_view{})),
        read_bio_(std::exchange(other.read_bio_, nullptr)),
        write_bio_(std::exchange(other.write_bio_, nullptr)) {}

  /**
   * Frees the current SSL object and moves ownership from another stream.
   */
  stream& operator=(stream&& other) noexcept {
    if (this != &other) {
      release();
      next_layer_ = std::move(other.next_layer_);
      ssl_ = std::exchange(other.ssl_, stream_view{});
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
    return bnio::get_lowest_layer(next_layer_);
  }

  /**
   * Returns the lowest layer for asynchronous socket operations.
   */
  [[nodiscard]] decltype(auto) lowest_layer() const noexcept {
    return bnio::get_lowest_layer(next_layer_);
  }

  /**
   * Returns a non-owning view of the owned native SSL object, or an invalid
   * view when this stream owns no connection.
   */
  [[nodiscard]] stream_view native_handle() const noexcept { return ssl_; }

  /**
   * Returns a non-owning view of the owned native SSL object, or an invalid
   * view when this stream owns no connection.
   */
  [[nodiscard]] stream_view get_native_handle() const noexcept {
    return native_handle();
  }

  /**
   * Returns whether this stream owns a native SSL object.
   */
  [[nodiscard]] bool valid() const noexcept { return ssl_.valid(); }

  /**
   * Replaces the context backing this stream's SSL object, mirroring
   * OpenSSL's SSL_set_SSL_CTX primitive.
   *
   * The owning ssl::context overload is the primary entry point; the
   * context_view overload is for callers that only hold a non-owning handle
   * (and for internal delegation). The stream keeps no bnio reference to the
   * context: SSL_set_SSL_CTX keeps the native SSL_CTX alive through the SSL
   * object's own reference, so the stream may safely outlive the owning
   * context. The owning context only has to outlive every stream whose
   * handshake should still invoke its ALPN/keylog callbacks; destroying it
   * clears those native registrations and the closures even while the
   * connection stays alive. Moving the context is safe because callback
   * registrations point at heap closure state owned by the context slot.
   * Calling it after the handshake may succeed, but it never retroactively
   * changes an already established connection.
   *
   * This is a low-level primitive only: it performs no context or certificate
   * selection and no SNI handling. It is not a reset: the SSL object and its
   * BIO pair are preserved, and no certificates are validated here. Expect to
   * call it before the handshake.
   *
   * Passing the current context is a successful no-op. On failure the
   * previous context and certificates remain in effect. Returns
   * make_no_ssl_error() when this stream owns no SSL object,
   * std::errc::invalid_argument when context is invalid (this is detected
   * before OpenSSL is called, because SSL_set_SSL_CTX(ssl, nullptr) silently
   * switches to the session context instead of failing), and the recorded
   * OpenSSL error when SSL_set_SSL_CTX fails.
   */
  [[nodiscard]] std::error_code set_context(
      bnio::ssl::context& context) noexcept {
    return set_context(context.view());
  }

  /**
   * Replaces the context backing this stream's SSL object from an owning base
   * context. Equivalent to the ssl::context overload.
   */
  [[nodiscard]] std::error_code set_context(
      base::context_base& context) noexcept {
    return set_context(context.view());
  }

  /**
   * Replaces the context backing this stream's SSL object from a non-owning
   * context view. Use this only when the caller already holds a non-owning
   * handle; passing the owning ssl::context is the primary entry point.
   */
  [[nodiscard]] std::error_code set_context(context_view context) noexcept {
    if (!valid()) {
      return base::make_no_ssl_error();
    }
    if (!context.valid()) {
      return std::make_error_code(std::errc::invalid_argument);
    }
    base::clear_errors();
    if (SSL_set_SSL_CTX(ssl_.native_handle(), context.native_handle()) ==
        nullptr) {
      return base::last_error();
    }
    return base::empty_error_code;
  }

  /**
   * Returns the transport BIO used for encrypted input.
   */
  [[nodiscard]] BIO* native_read_bio() const noexcept { return read_bio_; }

  /**
   * Returns the transport BIO used for encrypted output.
   */
  [[nodiscard]] BIO* native_write_bio() const noexcept { return write_bio_; }

  /**
   * Returns a non-owning read-only view of the ALPN protocol selected during
   * the handshake. The view is empty when no protocol was negotiated, when
   * the peer did not acknowledge ALPN, or when the handshake has not
   * completed. The bytes point into the SSL object's internal storage and
   * remain valid as long as this stream is alive; renegotiation may
   * change the contents. Use this to dispatch on the negotiated protocol.
   */
  [[nodiscard]] std::span<const unsigned char> get_alpn_selected()
      const noexcept {
    return ssl_.alpn_selected();
  }

  /**
   * Creates a handshake sender whose transport I/O uses the scheduler's queued
   * submission path.
   */
  template <class Scheduler>
  [[nodiscard]] auto async_handshake(Scheduler scheduler,
                                     bnio::ssl::handshake_type type);

  /**
   * Creates a handshake sender that runs the SSL attempt first and invokes
   * `factory` only when the attempt pauses.
   *
   * This is the full retry surface: the factory receives the pause code and
   * returns a sender (for example make_handshake_wait_sender) that performs
   * one wait step; an empty completion resumes the handshake, a non-empty
   * one becomes the terminal result, and set_stopped cancels. The receiver
   * contract is unchanged from the 2-argument overload: exactly
   * set_value(std::error_code) plus set_stopped(), and pause codes never
   * reach the public receiver. Everything is stored inline; connecting and
   * starting do not allocate. The factory is invoked zero times when the
   * stream is invalid or the attempt finishes without pausing.
   */
  template <class Scheduler, class Factory>
    requires detail::handshake_retry_factory<Factory>
  [[nodiscard]] auto async_handshake(Scheduler scheduler,
                                     bnio::ssl::handshake_type type,
                                     Factory&& factory);

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
   * Creates a shutdown sender whose transport I/O uses the scheduler's queued
   * submission path.
   */
  template <class Scheduler>
  [[nodiscard]] auto async_shutdown(Scheduler scheduler);

 private:
  void release() noexcept {
    if (ssl_.valid()) {
      SSL_free(ssl_.native_handle());
      ssl_ = stream_view{};
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

  void reset(bnio::ssl::context& context) noexcept { reset(context.view()); }

  void reset(base::context_base& context) noexcept { reset(context.view()); }

  void reset(context_view context) noexcept {
    ssl_ = stream_view(SSL_new(context.native_handle()));
    if (!ssl_.valid()) {
      return;
    }

    const base::bio_pair pair = base::bio_pair::make();
    if (!pair.valid()) {
      SSL_free(ssl_.native_handle());
      ssl_ = stream_view{};
      return;
    }

    SSL_set_bio(ssl_.native_handle(), pair.ssl_read, pair.ssl_write);
    read_bio_ = pair.transport_read;
    write_bio_ = pair.transport_write;
    SSL_set_mode(ssl_.native_handle(), SSL_MODE_ENABLE_PARTIAL_WRITE |
                                           SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  }

  NextLayer next_layer_;
  stream_view ssl_;
  BIO* read_bio_ = nullptr;
  BIO* write_bio_ = nullptr;
};

}  // namespace bnio::ssl::tcp

#endif  // BNIO_SSL_TCP_STREAM_CLASS_H_
