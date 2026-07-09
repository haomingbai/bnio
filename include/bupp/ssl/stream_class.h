#pragma once
#ifndef BUPP_SSL_STREAM_CLASS_H_
#define BUPP_SSL_STREAM_CLASS_H_

#include <bupp/buffer.h>
#include <bupp/io_context.h>
#include <bupp/ssl/context.h>
#include <bupp/tcp.h>
#include <openssl/ssl.h>

#include <cstddef>
#include <utility>

namespace bupp {

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

#endif  // BUPP_SSL_STREAM_CLASS_H_
