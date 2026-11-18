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
 * Owns the SSL object and the transport halves of its BIO pair, and stores
 * the next layer by value. Move-only because SSL ownership is unique.
 */
template <class NextLayer = bnio::tcp_socket>
class stream {
 public:
  /**
   * Creates a stream over next_layer; SSL_new takes its own SSL_CTX reference.
   * Dynamically looked-up callbacks tolerate an earlier context death, but a
   * cert callback needs the context to outlive the stream (set_cert_callback).
   */
  stream(NextLayer next_layer, bnio::ssl::context& context) noexcept
      : next_layer_(std::move(next_layer)) {
    reset(context);
  }

  /**
   * Same as the ssl::context overload; only the native SSL_CTX is used.
   */
  stream(NextLayer next_layer, base::context_base& context) noexcept
      : next_layer_(std::move(next_layer)) {
    reset(context);
  }

  /**
   * Creates a stream from a non-owning context view. Prefer the owning
   * ssl::context overload; SSL_new takes its own reference. Callback lifetime
   * matches the ssl::context overload.
   */
  stream(NextLayer next_layer, context_view context) noexcept
      : next_layer_(std::move(next_layer)) {
    reset(context);
  }

  /** Frees the owned SSL object and its BIOs. */
  ~stream() noexcept { release(); }

  /** Copy construction is disabled; SSL ownership is unique. */
  stream(const stream&) = delete;

  /** Copy assignment is disabled; SSL ownership is unique. */
  stream& operator=(const stream&) = delete;

  /** Moves SSL, BIO, and next-layer ownership from another stream. */
  stream(stream&& other) noexcept
      : next_layer_(std::move(other.next_layer_)),
        ssl_(std::exchange(other.ssl_, stream_view{})),
        read_bio_(std::exchange(other.read_bio_, nullptr)),
        write_bio_(std::exchange(other.write_bio_, nullptr)) {}

  /** Frees the current SSL object, then moves ownership from another stream. */
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

  /** Returns the stored next layer. */
  [[nodiscard]] NextLayer& next_layer() noexcept { return next_layer_; }

  /** Returns the stored next layer. */
  [[nodiscard]] const NextLayer& next_layer() const noexcept {
    return next_layer_;
  }

  /** Returns the lowest layer for asynchronous socket operations. */
  [[nodiscard]] decltype(auto) lowest_layer() noexcept {
    return bnio::get_lowest_layer(next_layer_);
  }

  /** Returns the lowest layer for asynchronous socket operations. */
  [[nodiscard]] decltype(auto) lowest_layer() const noexcept {
    return bnio::get_lowest_layer(next_layer_);
  }

  /** Returns a non-owning view of the owned SSL object; invalid when none. */
  [[nodiscard]] stream_view native_handle() const noexcept { return ssl_; }

  /** Alias of native_handle(). */
  [[nodiscard]] stream_view get_native_handle() const noexcept {
    return native_handle();
  }

  /** True when a native SSL object is owned. */
  [[nodiscard]] bool valid() const noexcept { return ssl_.valid(); }

  /**
   * SSL_set_SSL_CTX: swaps the context in place (SSL object and BIOs
   * preserved) without certificate selection, SNI, or validation; call it
   * before the handshake, as an established connection is not changed. A
   * failure keeps the previous context, and passing the current one is a
   * no-op.
   *
   * A target context with a certificate callback must outlive this stream
   * (SSL_set_SSL_CTX copies cert_cb_arg); ALPN/SNI/client_hello registrations
   * are looked up dynamically and are safe to clear.
   *
   * Returns make_no_ssl_error() without an SSL object, errc::invalid_argument
   * for an invalid view (OpenSSL would silently use the session context), else
   * the OpenSSL error.
   */
  [[nodiscard]] std::error_code set_context(
      bnio::ssl::context& context) noexcept {
    return set_context(context.view());
  }

  /** Same as the ssl::context overload. */
  [[nodiscard]] std::error_code set_context(
      base::context_base& context) noexcept {
    return set_context(context.view());
  }

  /**
   * Non-owning overload; prefer the ssl::context one. Same contract.
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

  /** Returns the transport BIO carrying encrypted input. */
  [[nodiscard]] BIO* native_read_bio() const noexcept { return read_bio_; }

  /** Returns the transport BIO carrying encrypted output. */
  [[nodiscard]] BIO* native_write_bio() const noexcept { return write_bio_; }

  /**
   * Returns the protocol selected by ALPN, or an empty span when none was
   * negotiated, the peer did not acknowledge ALPN, or the handshake has not
   * completed. Bytes point into the SSL object and stay valid while this
   * stream lives; renegotiation may change them.
   */
  [[nodiscard]] std::span<const unsigned char> get_alpn_selected()
      const noexcept {
    return ssl_.alpn_selected();
  }

  /**
   * Handshake sender; callback-driven pauses complete with
   * errc::operation_not_supported because this overload has no resume surface.
   */
  template <class Scheduler>
  [[nodiscard]] auto async_handshake(Scheduler scheduler,
                                     bnio::ssl::handshake_type type);

  /**
   * Like the 2-argument overload, but callback-driven pauses invoke `factory`
   * with the pause code and wait on its sender: an empty completion resumes, a
   * non-empty one is terminal, and set_stopped cancels; pause codes never
   * reach the receiver. The receiver contract is set_value(std::error_code)
   * plus set_stopped(); transport pauses are driven internally.
   */
  template <class Scheduler, class Factory>
    requires detail::handshake_retry_factory<Factory>
  [[nodiscard]] auto async_handshake(Scheduler scheduler,
                                     bnio::ssl::handshake_type type,
                                     Factory&& factory);

  /** Creates a sender for one plaintext read; may complete with fewer bytes. */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_read(Scheduler scheduler, Buffer&& buffer,
                                int flags = 0);

  /** Explicit read-some spelling of async_read; same contract. */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_read_some(Scheduler scheduler, Buffer&& buffer,
                                     int flags = 0);

  /** Creates a sender that writes the whole plaintext buffer through SSL. */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_write(Scheduler scheduler, Buffer&& buffer,
                                 int flags = 0);

  /** Creates a sender for one SSL_write step without retrying short writes. */
  template <class Scheduler, class Buffer>
  [[nodiscard]] auto async_write_some(Scheduler scheduler, Buffer&& buffer,
                                      int flags = 0);

  /**
   * Creates a shutdown sender using the scheduler's queued submission path.
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
