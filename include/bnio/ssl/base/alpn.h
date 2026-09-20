/**
 * @file alpn.h
 * @brief Destruction interface for context-owned ALPN callback state.
 */

#pragma once
#ifndef BNIO_SSL_BASE_ALPN_H_
#define BNIO_SSL_BASE_ALPN_H_

namespace bnio::ssl::base {

class context_base;

/**
 * Destruction interface for callback state owned by context_base. Concrete
 * argument types know their own allocator and implement destroy() to destroy
 * and deallocate themselves; context_base only ever calls destroy() and never
 * deletes through this base pointer.
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
