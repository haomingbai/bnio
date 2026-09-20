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
#include <bnio/ssl/context.h>
#include <bnio/tcp.h>
#include <openssl/ssl.h>

#include <cstddef>
#include <span>
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
   * Creates an SSL stream over a next layer using an existing context.
   */
  stream(NextLayer next_layer, base::context_base& context) noexcept
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
        ssl_(std::exchange(other.ssl_, nullptr)),
        read_bio_(std::exchange(other.read_bio_, nullptr)),
        write_bio_(std::exchange(other.write_bio_, nullptr)) {}

  /**
   * Frees the current SSL object and moves ownership from another stream.
   */
  stream& operator=(stream&& other) noexcept {
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
    return bnio::get_lowest_layer(next_layer_);
  }

  /**
   * Returns the lowest layer for asynchronous socket operations.
   */
  [[nodiscard]] decltype(auto) lowest_layer() const noexcept {
    return bnio::get_lowest_layer(next_layer_);
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
   * Returns a non-owning read-only view of the ALPN protocol selected during
   * the handshake. The view is empty when no protocol was negotiated, when
   * the peer did not acknowledge ALPN, or when the handshake has not
   * completed. The bytes point into the SSL object's internal storage and
   * remain valid as long as this stream is alive; renegotiation may
   * change the contents. Use this to dispatch on the negotiated protocol.
   */
  [[nodiscard]] std::span<const unsigned char> get_alpn_selected()
      const noexcept {
    const unsigned char* data = nullptr;
    unsigned int len = 0;
    SSL_get0_alpn_selected(ssl_, &data, &len);
    return {data, len};
  }

  /**
   * Creates a handshake sender whose transport I/O uses the scheduler's queued
   * submission path.
   */
  template <class Scheduler>
  [[nodiscard]] auto async_handshake(
      Scheduler scheduler, bnio::ssl::handshake_type type);

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

  void reset(base::context_base& context) noexcept {
    ssl_ = SSL_new(context.native_handle());
    if (ssl_ == nullptr) {
      return;
    }

    const base::bio_pair pair = base::bio_pair::make();
    if (!pair.valid()) {
      SSL_free(ssl_);
      ssl_ = nullptr;
      return;
    }

    SSL_set_bio(ssl_, pair.ssl_read, pair.ssl_write);
    read_bio_ = pair.transport_read;
    write_bio_ = pair.transport_write;
    SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE |
                           SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  }

  NextLayer next_layer_;
  SSL* ssl_ = nullptr;
  BIO* read_bio_ = nullptr;
  BIO* write_bio_ = nullptr;
};

}  // namespace bnio::ssl::tcp

#endif  // BNIO_SSL_TCP_STREAM_CLASS_H_
