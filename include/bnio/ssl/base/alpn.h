/**
 * @file alpn.h
 * @brief Destruction interface for heap-allocated callback state.
 */

#pragma once
#ifndef BNIO_SSL_BASE_ALPN_H_
#define BNIO_SSL_BASE_ALPN_H_

namespace bnio::ssl::base {

/**
 * Destruction interface for heap-allocated callbacks.
 *
 * Legacy, currently unused: no bnio type derives from alpn_arg_base. The
 * high-level callback closures are owned through detail::callback_base
 * (include/bnio/ssl/detail/callback_slot.h). This declaration is kept only
 * for header compatibility; new callback state must not build on it.
 *
 * Concrete argument types know their own allocator and implement destroy() to
 * destroy and deallocate themselves; an owner only ever calls destroy() and
 * never deletes through this base pointer.
 */
class alpn_arg_base {
 public:
  /**
   * Destroys and deallocates the concrete argument object.
   */
  virtual void destroy() noexcept = 0;

 protected:
  // Protected non-virtual destructor: destruction always goes through
  // destroy(), never through a base-class delete.
  alpn_arg_base() = default;
  ~alpn_arg_base() = default;
};

}  // namespace bnio::ssl::base

#endif  // BNIO_SSL_BASE_ALPN_H_
