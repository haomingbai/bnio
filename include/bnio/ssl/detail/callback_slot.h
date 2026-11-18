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
 * Concrete closure types know their own allocator and implement destroy() to
 * destroy and deallocate themselves; the owner only ever calls destroy() and
 * never deletes through this base pointer.
 */
class callback_base {
 public:
  /**
   * Destroys and deallocates the concrete closure object.
   */
  virtual void destroy() noexcept = 0;

 protected:
  // Protected non-virtual destructor: destruction always goes through
  // destroy(), never through a base-class delete.
  callback_base() = default;
  ~callback_base() = default;
};

/**
 * Inline set of callback closures owned by one ssl::context.
 *
 * The slot stores raw ownership pointers so a context move transfers them
 * without allocating. It is deliberately never handed to OpenSSL: every
 * currently implemented channel (ALPN, servername, client_hello, cert)
 * installs the heap closure pointer directly as the native callback's void*
 * argument. The keylog, verify, and info members are reserved for future
 * argument-less callbacks; those would look their closure up through a
 * private ex_data index instead of a native void* argument. The msg member is
 * also reserved, but it is not argument-less: OpenSSL's msg_callback_arg is
 * copied by SSL_new, so it needs a different lifetime strategy. A context
 * move therefore changes neither the native registrations nor the closure
 * addresses.
 *
 * Copying is disabled because each closure has a single owner. Moving nulls
 * the source so it no longer destroys the transferred closures. Reserved
 * slots are never populated yet and stay null.
 */
class callback_slot {
 public:
  /**
   * Creates an empty slot owning no closures.
   */
  callback_slot() noexcept = default;

  /**
   * Destroys every owned closure.
   */
  ~callback_slot() noexcept { reset(); }

  /**
   * Copy construction is disabled because closures have a single owner.
   */
  callback_slot(const callback_slot&) = delete;

  /**
   * Copy assignment is disabled because closures have a single owner.
   */
  callback_slot& operator=(const callback_slot&) = delete;

  /**
   * Transfers closure ownership from another slot and leaves it empty.
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
   * Destroys the current closures and transfers ownership from another slot.
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
   * Destroys every owned closure and leaves the slot empty.
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
   * Owned ALPN closure, or nullptr when no high-level callback is installed.
   */
  callback_base* alpn = nullptr;

  /**
   * Owned SNI servername callback closure, or nullptr when no servername
   * callback is installed.
   */
  callback_base* servername = nullptr;

  /**
   * Owned client_hello callback closure, or nullptr when no client_hello
   * callback is installed.
   */
  callback_base* client_hello = nullptr;

  /**
   * Owned per-connection certificate callback closure, or nullptr when no
   * certificate callback is installed.
   */
  callback_base* cert = nullptr;

  /**
   * Reserved for a future owned keylog closure; currently always nullptr.
   */
  callback_base* keylog = nullptr;

  /**
   * Reserved for a future owned certificate-verification closure; currently
   * always nullptr.
   */
  callback_base* verify = nullptr;

  /**
   * Reserved for a future owned info callback closure; currently always
   * nullptr.
   */
  callback_base* info = nullptr;

  /**
   * Reserved for a future owned message callback closure; currently always
   * nullptr.
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
