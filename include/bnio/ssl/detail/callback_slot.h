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
 * without allocating. It is deliberately never handed to OpenSSL: the only
 * channel currently implemented (ALPN) installs the heap closure pointer
 * directly as the native callback's void* argument. The keylog, verify, info,
 * and msg members are reserved for future argument-less callbacks; those
 * would look their closure up through a private ex_data index instead of a
 * native void* argument. A context move therefore changes neither the native
 * registrations nor the closure addresses.
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
   * Reserved for a future owned keylog closure; currently always nullptr
   * (only the ALPN closure channel is implemented).
   */
  callback_base* keylog = nullptr;

  /**
   * Reserved for a future owned certificate-verification closure; currently
   * always nullptr (only the ALPN closure channel is implemented).
   */
  callback_base* verify = nullptr;

  /**
   * Reserved for a future owned info callback closure; currently always
   * nullptr (only the ALPN closure channel is implemented).
   */
  callback_base* info = nullptr;

  /**
   * Reserved for a future owned message callback closure; currently always
   * nullptr (only the ALPN closure channel is implemented).
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
