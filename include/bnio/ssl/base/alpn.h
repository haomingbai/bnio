/**
 * @file alpn.h
 * @brief Destruction interface for heap-allocated callback state.
 */

#pragma once
#ifndef BNIO_SSL_BASE_ALPN_H_
#define BNIO_SSL_BASE_ALPN_H_

namespace bnio::ssl::base {

/**
 * Legacy, unused destruction interface kept for header compatibility; use
 * detail::callback_base for new callback state. Destruction always goes
 * through destroy(), never a base-class delete.
 */
class alpn_arg_base {
 public:
  /** Destroys and deallocates the concrete argument object. */
  virtual void destroy() noexcept = 0;

 protected:
  // Protected non-virtual destructor: destruction always goes through
  // destroy(), never through a base-class delete.
  alpn_arg_base() = default;
  ~alpn_arg_base() = default;
};

}  // namespace bnio::ssl::base

#endif  // BNIO_SSL_BASE_ALPN_H_
