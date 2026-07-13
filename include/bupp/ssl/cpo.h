#pragma once
#ifndef BUPP_SSL_CPO_H_
#define BUPP_SSL_CPO_H_

#include <bupp/ssl/context.h>

#include <utility>

namespace bupp {

/**
 * Customization point object for Provider::async_handshake.
 */
struct async_handshake_t {
  /**
   * Invokes async_handshake on a stream.
   */
  template <class Provider, class Stream>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      ssl_handshake_type type) const {
    return std::forward<Stream>(stream).async_handshake(
        std::forward<Provider>(provider), type);
  }
};

/**
 * Customization point object for Provider::async_shutdown.
 */
struct async_shutdown_t {
  /**
   * Invokes async_shutdown on a stream.
   */
  template <class Provider, class Stream>
  constexpr decltype(auto) operator()(Provider&& provider,
                                      Stream&& stream) const {
    return std::forward<Stream>(stream).async_shutdown(
        std::forward<Provider>(provider));
  }
};

/**
 * Customization point object instance for async_handshake.
 */
inline constexpr async_handshake_t async_handshake{};

/**
 * Customization point object instance for async_shutdown.
 */
inline constexpr async_shutdown_t async_shutdown{};

}  // namespace bupp

#endif  // BUPP_SSL_CPO_H_
