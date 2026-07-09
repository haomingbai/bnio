#pragma once
#ifndef BUPP_IO_CONTEXT_CPO_CONNECTION_H_
#define BUPP_IO_CONTEXT_CPO_CONNECTION_H_

#include <utility>

namespace bupp {

/**
 * Customization point object for Provider::async_accept.
 */
struct async_accept_t {
  /**
   * Invokes async_accept on an acceptor when available, otherwise on a
   * provider.
   */
  template <class Provider, class Acceptor>
  constexpr decltype(auto) operator()(Provider&& provider, Acceptor&& acceptor,
                                      int flags = 0) const {
    if constexpr (requires {
                    std::forward<Acceptor>(acceptor).async_accept(
                        std::forward<Provider>(provider), flags);
                  }) {
      return std::forward<Acceptor>(acceptor).async_accept(
          std::forward<Provider>(provider), flags);
    } else {
      return std::forward<Provider>(provider).async_accept(
          std::forward<Acceptor>(acceptor), flags);
    }
  }
};

/**
 * Customization point object for direct-submission accept.
 */
struct async_accept_direct_t {
  /**
   * Invokes async_accept_direct on an acceptor when that direct-submission
   * customization exists, otherwise on a provider.
   */
  template <class Provider, class Acceptor>
  constexpr decltype(auto) operator()(Provider&& provider, Acceptor&& acceptor,
                                      int flags = 0) const {
    if constexpr (requires {
                    std::forward<Acceptor>(acceptor).async_accept_direct(
                        std::forward<Provider>(provider), flags);
                  }) {
      return std::forward<Acceptor>(acceptor).async_accept_direct(
          std::forward<Provider>(provider), flags);
    } else {
      return std::forward<Provider>(provider).async_accept_direct(
          std::forward<Acceptor>(acceptor), flags);
    }
  }
};

/**
 * Customization point object for Provider::async_connect.
 */
struct async_connect_t {
  /**
   * Invokes async_connect on a stream when available, otherwise on a provider.
   */
  template <class Provider, class Stream, class Endpoint>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      Endpoint&& endpoint) const {
    if constexpr (requires {
                    std::forward<Stream>(stream).async_connect(
                        std::forward<Provider>(provider),
                        std::forward<Endpoint>(endpoint));
                  }) {
      return std::forward<Stream>(stream).async_connect(
          std::forward<Provider>(provider), std::forward<Endpoint>(endpoint));
    } else {
      return std::forward<Provider>(provider).async_connect(
          std::forward<Stream>(stream), std::forward<Endpoint>(endpoint));
    }
  }
};

/**
 * Customization point object for direct-submission connect.
 */
struct async_connect_direct_t {
  /**
   * Invokes async_connect_direct on a stream when that direct-submission
   * customization exists, otherwise on a provider.
   */
  template <class Provider, class Stream, class Endpoint>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      Endpoint&& endpoint) const {
    if constexpr (requires {
                    std::forward<Stream>(stream).async_connect_direct(
                        std::forward<Provider>(provider),
                        std::forward<Endpoint>(endpoint));
                  }) {
      return std::forward<Stream>(stream).async_connect_direct(
          std::forward<Provider>(provider), std::forward<Endpoint>(endpoint));
    } else {
      return std::forward<Provider>(provider).async_connect_direct(
          std::forward<Stream>(stream), std::forward<Endpoint>(endpoint));
    }
  }
};

}  // namespace bupp

#endif  // BUPP_IO_CONTEXT_CPO_CONNECTION_H_
