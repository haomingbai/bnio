#pragma once
#ifndef BUPP_IO_CONTEXT_CPO_H_
#define BUPP_IO_CONTEXT_CPO_H_

#include <bexec/scheduler.hpp>
#include <bexec/sender.hpp>
#include <type_traits>
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
 * Concept satisfied when a provider returns a sender from async_receive.
 */
template <class Scheduler, class Stream, class Buffer>
concept receives_bytes =
    bexec::scheduler<std::remove_cvref_t<Scheduler>> &&
    requires(Scheduler& scheduler, Stream& stream, Buffer&& buffer) {
      {
        async_receive(scheduler, stream, std::forward<Buffer>(buffer))
      } -> bexec::sender;
    };

/**
 * Concept satisfied when a provider returns a sender from async_send.
 */
template <class Scheduler, class Stream, class Buffer>
concept sends_bytes =
    bexec::scheduler<std::remove_cvref_t<Scheduler>> &&
    requires(Scheduler& scheduler, Stream& stream, Buffer&& buffer) {
      {
        async_send(scheduler, stream, std::forward<Buffer>(buffer))
      } -> bexec::sender;
    };

/**
 * Concept satisfied when a provider returns a sender from async_accept.
 */
template <class Scheduler, class Acceptor>
concept accepts_connections =
    bexec::scheduler<std::remove_cvref_t<Scheduler>> &&
    requires(Scheduler& scheduler, Acceptor& acceptor) {
      { async_accept(scheduler, acceptor) } -> bexec::sender;
    };

/**
 * Concept satisfied when a scheduler returns a sender from async_connect.
 */
template <class Scheduler, class Stream, class Endpoint>
concept connects_stream =
    bexec::scheduler<std::remove_cvref_t<Scheduler>> &&
    requires(Scheduler& scheduler, Stream& stream, Endpoint&& endpoint) {
      {
        async_connect(scheduler, stream, std::forward<Endpoint>(endpoint))
      } -> bexec::sender;
};

}  // namespace bupp

#endif  // BUPP_IO_CONTEXT_CPO_H_
