/**
 * @file bio_pair.h
 * @brief Minimal assembly facility for the SSL memory BIO pair.
 */

#pragma once
#ifndef BNIO_SSL_BASE_BIO_PAIR_H_
#define BNIO_SSL_BASE_BIO_PAIR_H_

#include <openssl/bio.h>

#include <cstddef>

namespace bnio::ssl::base {

/**
 * Capacity of each memory BIO half that buffers application-facing payload
 * between the SSL object and the transport.
 */
inline constexpr std::size_t bio_buffer_capacity = 64 * 1024;

/**
 * The two memory BIO pairs wiring an SSL object to a byte transport.
 *
 * The ssl-side halves are handed to SSL_set_bio and are freed when the SSL
 * object is freed; the transport-side halves are owned by the caller (the
 * stream) and must be freed explicitly. This is the smallest assembly for the
 * memory-BIO plumbing that used to live inline in stream construction; the
 * transport concept (encrypted in on transport_read, encrypted out on
 * transport_write) stays stable, so a future transport implementation can be
 * swapped in without changing the shape of this facility.
 */
class bio_pair {
 public:
  BIO* ssl_read = nullptr;
  BIO* ssl_write = nullptr;
  BIO* transport_read = nullptr;
  BIO* transport_write = nullptr;

  /**
   * Assembles the two BIO pairs. All-or-nothing: if either pair cannot be
   * created, every half is returned as nullptr. The ssl_read pair buffers
   * decrypted input toward the application with an unbounded ssl side; the
   * ssl_write pair buffers encrypted output from the SSL object with a
   * bounded ssl side (bio_buffer_capacity). The transport concept (encrypted
   * in on transport_read, encrypted out on transport_write) stays stable, so
   * a future transport layer can replace the current wiring without touching
   * this assembly.
   */
  static bio_pair make() noexcept {
    bio_pair pair;
    if (BIO_new_bio_pair(&pair.ssl_read, 0, &pair.transport_read,
                         bio_buffer_capacity) != 1) {
      return {};
    }
    if (BIO_new_bio_pair(&pair.ssl_write, bio_buffer_capacity,
                         &pair.transport_write, 0) != 1) {
      // Release the already-created first pair so no half leaks.
      BIO_free(pair.ssl_read);
      BIO_free(pair.transport_read);
      return {};
    }
    return pair;
  }

  /**
   * Returns whether all four BIO halves were created.
   */
  [[nodiscard]] bool valid() const noexcept {
    return ssl_read != nullptr && ssl_write != nullptr &&
           transport_read != nullptr && transport_write != nullptr;
  }

  /**
   * Frees the caller-owned transport halves (encrypted in and encrypted out).
   */
  void release_transport_halves() noexcept {
    BIO_free(transport_read);
    transport_read = nullptr;
    BIO_free(transport_write);
    transport_write = nullptr;
  }
};

}  // namespace bnio::ssl::base

#endif  // BNIO_SSL_BASE_BIO_PAIR_H_
