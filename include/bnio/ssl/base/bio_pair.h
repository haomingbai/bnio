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
 * Capacity of each memory BIO half buffering payload between SSL and the
 * transport.
 */
inline constexpr std::size_t bio_buffer_capacity = 64 * 1024;

/**
 * The two memory BIO pairs wiring an SSL object to a byte transport.
 *
 * The ssl-side halves are freed with the SSL object; the transport-side
 * halves are caller-owned and must be freed explicitly.
 */
class bio_pair {
 public:
  /**
   * SSL-side read half; freed with the SSL object. Pairs with transport_read.
   */
  BIO* ssl_read = nullptr;

  /**
   * SSL-side write half; bounded by bio_buffer_capacity and freed with the SSL
   * object. Pairs with transport_write.
   */
  BIO* ssl_write = nullptr;

  /**
   * Caller-owned read half: encrypted bytes received from the peer are written
   * here and become readable by the SSL object via ssl_read. Free explicitly.
   */
  BIO* transport_read = nullptr;

  /**
   * Caller-owned write half: encrypted output from the SSL object becomes
   * readable here via ssl_write and is sent to the peer. Free explicitly.
   */
  BIO* transport_write = nullptr;

  /**
   * Assembles both pairs; all-or-nothing, returning a pair with all halves
   * null if either creation fails. The ssl side of the read pair is unbounded;
   * the ssl side of the write pair is bounded by bio_buffer_capacity.
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

  /** True when all four BIO halves were created. */
  [[nodiscard]] bool valid() const noexcept {
    return ssl_read != nullptr && ssl_write != nullptr &&
           transport_read != nullptr && transport_write != nullptr;
  }

  /** Frees the caller-owned transport halves (encrypted in and out). */
  void release_transport_halves() noexcept {
    BIO_free(transport_read);
    transport_read = nullptr;
    BIO_free(transport_write);
    transport_write = nullptr;
  }
};

}  // namespace bnio::ssl::base

#endif  // BNIO_SSL_BASE_BIO_PAIR_H_
