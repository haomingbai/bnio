/**
 * @file callback_slot.h
 * @brief Inline ownership slots for context callbacks.
 */

#pragma once
#ifndef BNIO_SSL_DETAIL_CALLBACK_SLOT_H_
#define BNIO_SSL_DETAIL_CALLBACK_SLOT_H_

#include <utility>

namespace bnio::ssl::detail {

/**
 * Destruction interface for heap-allocated callback closures.
 *
 * The owner only calls destroy(); it never deletes through this base pointer.
 */
class callback_base {
 public:
  /**
   * Destroys and deallocates the concrete closure object.
   */
  virtual void destroy() noexcept = 0;

 protected:
  // Protected non-virtual destructor: destruction goes through destroy(),
  // never through a base-class delete.
  callback_base() = default;
  ~callback_base() = default;
};

/**
 * Inline set of closure pointers owned by one ssl::context.
 *
 * Raw pointers let a move transfer ownership without rebinding native
 * registrations; copying is disabled and moving nulls the source.
 */
class callback_slot {
 public:
  /**
   * Creates an empty slot.
   */
  callback_slot() noexcept = default;

  /**
   * Destroys every owned closure.
   */
  ~callback_slot() noexcept { reset(); }

  /**
   * Disabled: each closure has a single owner.
   */
  callback_slot(const callback_slot&) = delete;

  /**
   * Disabled: each closure has a single owner.
   */
  callback_slot& operator=(const callback_slot&) = delete;

  /**
   * Transfers ownership from another slot and leaves it empty.
   */
  callback_slot(callback_slot&& other) noexcept
      : alpn(std::exchange(other.alpn, nullptr)),
        servername(std::exchange(other.servername, nullptr)),
        client_hello(std::exchange(other.client_hello, nullptr)),
        cert(std::exchange(other.cert, nullptr)),
        keylog(std::exchange(other.keylog, nullptr)),
        verify(std::exchange(other.verify, nullptr)),
        info(std::exchange(other.info, nullptr)),
        msg(std::exchange(other.msg, nullptr)) {}

  /**
   * Destroys the current closures and takes ownership from another slot.
   */
  callback_slot& operator=(callback_slot&& other) noexcept {
    if (this != &other) {
      reset();
      alpn = std::exchange(other.alpn, nullptr);
      servername = std::exchange(other.servername, nullptr);
      client_hello = std::exchange(other.client_hello, nullptr);
      cert = std::exchange(other.cert, nullptr);
      keylog = std::exchange(other.keylog, nullptr);
      verify = std::exchange(other.verify, nullptr);
      info = std::exchange(other.info, nullptr);
      msg = std::exchange(other.msg, nullptr);
    }
    return *this;
  }

  /**
   * Destroys every owned closure; the slot stays empty.
   */
  void reset() noexcept {
    destroy(alpn);
    destroy(servername);
    destroy(client_hello);
    destroy(cert);
    destroy(keylog);
    destroy(verify);
    destroy(info);
    destroy(msg);
  }

  /**
   * Owned ALPN closure, or nullptr.
   */
  callback_base* alpn = nullptr;

  /**
   * Owned SNI servername closure, or nullptr.
   */
  callback_base* servername = nullptr;

  /**
   * Owned client_hello closure, or nullptr.
   */
  callback_base* client_hello = nullptr;

  /**
   * Owned certificate closure, or nullptr.
   */
  callback_base* cert = nullptr;

  /**
   * Reserved for a future keylog closure; always nullptr.
   */
  callback_base* keylog = nullptr;

  /**
   * Reserved for a future certificate-verification closure; always nullptr.
   */
  callback_base* verify = nullptr;

  /**
   * Reserved for a future info callback closure; always nullptr.
   */
  callback_base* info = nullptr;

  /**
   * Reserved for a future message callback closure; always nullptr.
   */
  callback_base* msg = nullptr;

 private:
  static void destroy(callback_base*& closure) noexcept {
    if (closure != nullptr) {
      callback_base* owned = std::exchange(closure, nullptr);
      owned->destroy();
    }
  }
};

}  // namespace bnio::ssl::detail

#endif  // BNIO_SSL_DETAIL_CALLBACK_SLOT_H_
