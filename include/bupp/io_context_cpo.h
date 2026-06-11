#pragma once
#ifndef BUPP_IO_CONTEXT_CPO_H_
#define BUPP_IO_CONTEXT_CPO_H_

#include <bexec/sender.hpp>
#include <utility>

namespace bupp {

/**
 * Customization point object for Provider::async_receive.
 */
struct async_receive_t {
  /**
   * Invokes async_receive on a provider.
   */
  template <class Provider, class Stream, class Buffer>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      Buffer&& buffer) const
      noexcept(noexcept(std::forward<Provider>(provider).async_receive(
          std::forward<Stream>(stream), std::forward<Buffer>(buffer)))) {
    return std::forward<Provider>(provider).async_receive(
        std::forward<Stream>(stream), std::forward<Buffer>(buffer));
  }
};

/**
 * Customization point object for Provider::async_receive_direct.
 */
struct async_receive_direct_t {
  /**
   * Invokes async_receive_direct on a provider.
   */
  template <class Provider, class Stream, class Buffer>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      Buffer&& buffer) const
      noexcept(noexcept(std::forward<Provider>(provider).async_receive_direct(
          std::forward<Stream>(stream), std::forward<Buffer>(buffer)))) {
    return std::forward<Provider>(provider).async_receive_direct(
        std::forward<Stream>(stream), std::forward<Buffer>(buffer));
  }
};

/**
 * Customization point object for Provider::async_send.
 */
struct async_send_t {
  /**
   * Invokes async_send on a provider.
   */
  template <class Provider, class Stream, class Buffer>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      Buffer&& buffer) const
      noexcept(noexcept(std::forward<Provider>(provider).async_send(
          std::forward<Stream>(stream), std::forward<Buffer>(buffer)))) {
    return std::forward<Provider>(provider).async_send(
        std::forward<Stream>(stream), std::forward<Buffer>(buffer));
  }
};

/**
 * Customization point object for Provider::async_send_direct.
 */
struct async_send_direct_t {
  /**
   * Invokes async_send_direct on a provider.
   */
  template <class Provider, class Stream, class Buffer>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      Buffer&& buffer) const
      noexcept(noexcept(std::forward<Provider>(provider).async_send_direct(
          std::forward<Stream>(stream), std::forward<Buffer>(buffer)))) {
    return std::forward<Provider>(provider).async_send_direct(
        std::forward<Stream>(stream), std::forward<Buffer>(buffer));
  }
};

/**
 * Customization point object for Provider::async_accept.
 */
struct async_accept_t {
  /**
   * Invokes async_accept on a provider.
   */
  template <class Provider, class Acceptor>
  constexpr decltype(auto) operator()(Provider&& provider,
                                      Acceptor&& acceptor) const
      noexcept(noexcept(std::forward<Provider>(provider).async_accept(
          std::forward<Acceptor>(acceptor)))) {
    return std::forward<Provider>(provider).async_accept(
        std::forward<Acceptor>(acceptor));
  }
};

/**
 * Customization point object for Provider::async_accept_direct.
 */
struct async_accept_direct_t {
  /**
   * Invokes async_accept_direct on a provider.
   */
  template <class Provider, class Acceptor>
  constexpr decltype(auto) operator()(Provider&& provider,
                                      Acceptor&& acceptor) const
      noexcept(noexcept(std::forward<Provider>(provider).async_accept_direct(
          std::forward<Acceptor>(acceptor)))) {
    return std::forward<Provider>(provider).async_accept_direct(
        std::forward<Acceptor>(acceptor));
  }
};

/**
 * Customization point object for Provider::async_connect.
 */
struct async_connect_t {
  /**
   * Invokes async_connect on a provider.
   */
  template <class Provider, class Stream, class Endpoint>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      Endpoint&& endpoint) const
      noexcept(noexcept(std::forward<Provider>(provider).async_connect(
          std::forward<Stream>(stream), std::forward<Endpoint>(endpoint)))) {
    return std::forward<Provider>(provider).async_connect(
        std::forward<Stream>(stream), std::forward<Endpoint>(endpoint));
  }
};

/**
 * Customization point object for Provider::async_connect_direct.
 */
struct async_connect_direct_t {
  /**
   * Invokes async_connect_direct on a provider.
   */
  template <class Provider, class Stream, class Endpoint>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      Endpoint&& endpoint) const
      noexcept(noexcept(std::forward<Provider>(provider).async_connect_direct(
          std::forward<Stream>(stream), std::forward<Endpoint>(endpoint)))) {
    return std::forward<Provider>(provider).async_connect_direct(
        std::forward<Stream>(stream), std::forward<Endpoint>(endpoint));
  }
};

/**
 * Customization point object for Provider::async_wait.
 */
struct async_wait_t {
  /**
   * Invokes async_wait on a provider.
   */
  template <class Provider, class Duration>
  constexpr decltype(auto) operator()(Provider&& provider,
                                      Duration&& timeout) const
      noexcept(noexcept(std::forward<Provider>(provider).async_wait(
          std::forward<Duration>(timeout)))) {
    return std::forward<Provider>(provider).async_wait(
        std::forward<Duration>(timeout));
  }
};

/**
 * Customization point object for Provider::async_wait_direct.
 */
struct async_wait_direct_t {
  /**
   * Invokes async_wait_direct on a provider.
   */
  template <class Provider, class Duration>
  constexpr decltype(auto) operator()(Provider&& provider,
                                      Duration&& timeout) const
      noexcept(noexcept(std::forward<Provider>(provider).async_wait_direct(
          std::forward<Duration>(timeout)))) {
    return std::forward<Provider>(provider).async_wait_direct(
        std::forward<Duration>(timeout));
  }
};

/**
 * Customization point object instance for async_receive.
 */
inline constexpr async_receive_t async_receive{};

/**
 * Customization point object instance for async_receive_direct.
 */
inline constexpr async_receive_direct_t async_receive_direct{};

/**
 * Customization point object instance for async_send.
 */
inline constexpr async_send_t async_send{};

/**
 * Customization point object instance for async_send_direct.
 */
inline constexpr async_send_direct_t async_send_direct{};

/**
 * Customization point object instance for async_accept.
 */
inline constexpr async_accept_t async_accept{};

/**
 * Customization point object instance for async_accept_direct.
 */
inline constexpr async_accept_direct_t async_accept_direct{};

/**
 * Customization point object instance for async_connect.
 */
inline constexpr async_connect_t async_connect{};

/**
 * Customization point object instance for async_connect_direct.
 */
inline constexpr async_connect_direct_t async_connect_direct{};

/**
 * Customization point object instance for async_wait.
 */
inline constexpr async_wait_t async_wait{};

/**
 * Customization point object instance for async_wait_direct.
 */
inline constexpr async_wait_direct_t async_wait_direct{};

/**
 * Concept satisfied when a provider returns a sender from async_receive.
 */
template <class Provider, class Stream, class Buffer>
concept receives_bytes =
    requires(Provider& provider, Stream& stream, Buffer&& buffer) {
      {
        async_receive(provider, stream, std::forward<Buffer>(buffer))
      } -> bexec::sender;
    };

/**
 * Concept satisfied when a provider returns a sender from async_send.
 */
template <class Provider, class Stream, class Buffer>
concept sends_bytes =
    requires(Provider& provider, Stream& stream, Buffer&& buffer) {
      {
        async_send(provider, stream, std::forward<Buffer>(buffer))
      } -> bexec::sender;
    };

/**
 * Concept satisfied when a provider returns a sender from async_accept.
 */
template <class Provider, class Acceptor>
concept accepts_connections = requires(Provider& provider, Acceptor& acceptor) {
  { async_accept(provider, acceptor) } -> bexec::sender;
};

}  // namespace bupp

#endif  // BUPP_IO_CONTEXT_CPO_H_
