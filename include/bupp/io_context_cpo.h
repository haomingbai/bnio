#pragma once
#ifndef BUPP_IO_CONTEXT_CPO_H_
#define BUPP_IO_CONTEXT_CPO_H_

#include <bupp/async_io/dns.h>

#include <bexec/scheduler.hpp>
#include <bexec/sender.hpp>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace bupp {

/**
 * Customization point object for Provider::async_receive.
 */
struct async_receive_t {
  /**
   * Invokes async_receive on a stream when available, otherwise on a provider.
   */
  template <class Provider, class Stream, class Buffer>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      Buffer&& buffer, int flags = 0) const {
    if constexpr (requires {
                    std::forward<Stream>(stream).async_receive(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer), flags);
                  }) {
      return std::forward<Stream>(stream).async_receive(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer),
          flags);
    } else {
      return std::forward<Provider>(provider).async_receive(
          std::forward<Stream>(stream), std::forward<Buffer>(buffer), flags);
    }
  }
};

/**
 * Customization point object for Provider::async_receive_direct.
 */
struct async_receive_direct_t {
  /**
   * Invokes async_receive_direct on a stream when available, otherwise on a
   * provider.
   */
  template <class Provider, class Stream, class Buffer>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      Buffer&& buffer, int flags = 0) const {
    if constexpr (requires {
                    std::forward<Stream>(stream).async_receive_direct(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer), flags);
                  }) {
      return std::forward<Stream>(stream).async_receive_direct(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer),
          flags);
    } else {
      return std::forward<Provider>(provider).async_receive_direct(
          std::forward<Stream>(stream), std::forward<Buffer>(buffer), flags);
    }
  }
};

/**
 * Customization point object for Provider::async_send.
 */
struct async_send_t {
  /**
   * Invokes async_send on a stream when available, otherwise on a provider.
   */
  template <class Provider, class Stream, class Buffer>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      Buffer&& buffer, int flags = 0) const {
    if constexpr (requires {
                    std::forward<Stream>(stream).async_send(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer), flags);
                  }) {
      return std::forward<Stream>(stream).async_send(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer),
          flags);
    } else {
      return std::forward<Provider>(provider).async_send(
          std::forward<Stream>(stream), std::forward<Buffer>(buffer), flags);
    }
  }
};

/**
 * Customization point object for Provider::async_send_direct.
 */
struct async_send_direct_t {
  /**
   * Invokes async_send_direct on a stream when available, otherwise on a
   * provider.
   */
  template <class Provider, class Stream, class Buffer>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      Buffer&& buffer, int flags = 0) const {
    if constexpr (requires {
                    std::forward<Stream>(stream).async_send_direct(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer), flags);
                  }) {
      return std::forward<Stream>(stream).async_send_direct(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer),
          flags);
    } else {
      return std::forward<Provider>(provider).async_send_direct(
          std::forward<Stream>(stream), std::forward<Buffer>(buffer), flags);
    }
  }
};

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
 * Customization point object for Provider::async_accept_direct.
 */
struct async_accept_direct_t {
  /**
   * Invokes async_accept_direct on an acceptor when available, otherwise on a
   * provider.
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
 * Customization point object for Provider::async_connect_direct.
 */
struct async_connect_direct_t {
  /**
   * Invokes async_connect_direct on a stream when available, otherwise on a
   * provider.
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

/**
 * Customization point object for Provider::async_read.
 */
struct async_read_t {
  /**
   * Invokes async_read on a provider.
   */
  template <class Provider, class Descriptor, class Buffer>
  constexpr decltype(auto) operator()(Provider&& provider,
                                      Descriptor&& descriptor, Buffer&& buffer,
                                      std::uint64_t offset = 0) const {
    return std::forward<Provider>(provider).async_read(
        std::forward<Descriptor>(descriptor), std::forward<Buffer>(buffer),
        offset);
  }
};

/**
 * Customization point object for Provider::async_read_direct.
 */
struct async_read_direct_t {
  /**
   * Invokes async_read_direct on a provider.
   */
  template <class Provider, class Descriptor, class Buffer>
  constexpr decltype(auto) operator()(Provider&& provider,
                                      Descriptor&& descriptor, Buffer&& buffer,
                                      std::uint64_t offset = 0) const {
    return std::forward<Provider>(provider).async_read_direct(
        std::forward<Descriptor>(descriptor), std::forward<Buffer>(buffer),
        offset);
  }
};

/**
 * Customization point object for Provider::async_write.
 */
struct async_write_t {
  /**
   * Invokes async_write on a provider.
   */
  template <class Provider, class Descriptor, class Buffer>
  constexpr decltype(auto) operator()(Provider&& provider,
                                      Descriptor&& descriptor, Buffer&& buffer,
                                      std::uint64_t offset = 0) const {
    return std::forward<Provider>(provider).async_write(
        std::forward<Descriptor>(descriptor), std::forward<Buffer>(buffer),
        offset);
  }
};

/**
 * Customization point object for Provider::async_write_direct.
 */
struct async_write_direct_t {
  /**
   * Invokes async_write_direct on a provider.
   */
  template <class Provider, class Descriptor, class Buffer>
  constexpr decltype(auto) operator()(Provider&& provider,
                                      Descriptor&& descriptor, Buffer&& buffer,
                                      std::uint64_t offset = 0) const {
    return std::forward<Provider>(provider).async_write_direct(
        std::forward<Descriptor>(descriptor), std::forward<Buffer>(buffer),
        offset);
  }
};

/**
 * Customization point object for Provider::async_poll.
 */
struct async_poll_t {
  /**
   * Invokes async_poll on a provider.
   */
  template <class Provider, class Descriptor, class PollMask>
  constexpr decltype(auto) operator()(Provider&& provider,
                                      Descriptor&& descriptor,
                                      PollMask&& poll_mask) const
      noexcept(noexcept(std::forward<Provider>(provider).async_poll(
          std::forward<Descriptor>(descriptor),
          std::forward<PollMask>(poll_mask)))) {
    return std::forward<Provider>(provider).async_poll(
        std::forward<Descriptor>(descriptor),
        std::forward<PollMask>(poll_mask));
  }
};

/**
 * Customization point object for Provider::async_poll_direct.
 */
struct async_poll_direct_t {
  /**
   * Invokes async_poll_direct on a provider.
   */
  template <class Provider, class Descriptor, class PollMask>
  constexpr decltype(auto) operator()(Provider&& provider,
                                      Descriptor&& descriptor,
                                      PollMask&& poll_mask) const
      noexcept(noexcept(std::forward<Provider>(provider).async_poll_direct(
          std::forward<Descriptor>(descriptor),
          std::forward<PollMask>(poll_mask)))) {
    return std::forward<Provider>(provider).async_poll_direct(
        std::forward<Descriptor>(descriptor),
        std::forward<PollMask>(poll_mask));
  }
};

/**
 * Customization point object for Provider::async_resolve.
 */
struct async_resolve_t {
  /**
   * Invokes async_resolve on a provider with an owned query object and result
   * view.
   */
  template <class Provider, class Query, class ResultView>
  constexpr decltype(auto) operator()(Provider&& provider, Query&& query,
                                      ResultView&& result) const
      noexcept(noexcept(std::forward<Provider>(provider).async_resolve(
          std::forward<Query>(query), std::forward<ResultView>(result)))) {
    return std::forward<Provider>(provider).async_resolve(
        std::forward<Query>(query), std::forward<ResultView>(result));
  }

  /**
   * Invokes async_resolve on a provider with host, service, and result view
   * arguments.
   */
  template <class Provider, class Host, class Service, class ResultView>
  constexpr decltype(auto) operator()(Provider&& provider, Host&& host,
                                      Service&& service,
                                      ResultView&& result) const
      noexcept(noexcept(std::forward<Provider>(provider).async_resolve(
          std::forward<Host>(host), std::forward<Service>(service),
          std::forward<ResultView>(result)))) {
    return std::forward<Provider>(provider).async_resolve(
        std::forward<Host>(host), std::forward<Service>(service),
        std::forward<ResultView>(result));
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
 * Customization point object instance for async_read.
 */
inline constexpr async_read_t async_read{};

/**
 * Customization point object instance for async_read_direct.
 */
inline constexpr async_read_direct_t async_read_direct{};

/**
 * Customization point object instance for async_write.
 */
inline constexpr async_write_t async_write{};

/**
 * Customization point object instance for async_write_direct.
 */
inline constexpr async_write_direct_t async_write_direct{};

/**
 * Customization point object instance for async_poll.
 */
inline constexpr async_poll_t async_poll{};

/**
 * Customization point object instance for async_poll_direct.
 */
inline constexpr async_poll_direct_t async_poll_direct{};

/**
 * Customization point object instance for async_resolve.
 */
inline constexpr async_resolve_t async_resolve{};

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

/**
 * Concept satisfied when a scheduler returns a sender from async_read.
 */
template <class Scheduler, class Descriptor, class Buffer>
concept reads_descriptor =
    bexec::scheduler<std::remove_cvref_t<Scheduler>> &&
    requires(Scheduler& scheduler, Descriptor& descriptor, Buffer&& buffer) {
      {
        async_read(scheduler, descriptor, std::forward<Buffer>(buffer))
      } -> bexec::sender;
    };

/**
 * Concept satisfied when a scheduler returns a sender from async_write.
 */
template <class Scheduler, class Descriptor, class Buffer>
concept writes_descriptor =
    bexec::scheduler<std::remove_cvref_t<Scheduler>> &&
    requires(Scheduler& scheduler, Descriptor& descriptor, Buffer&& buffer) {
      {
        async_write(scheduler, descriptor, std::forward<Buffer>(buffer))
      } -> bexec::sender;
    };

/**
 * Concept satisfied when a scheduler returns a sender from async_poll.
 */
template <class Scheduler, class Descriptor>
concept polls_descriptor =
    bexec::scheduler<std::remove_cvref_t<Scheduler>> &&
    requires(Scheduler& scheduler, Descriptor& descriptor, unsigned poll_mask) {
      { async_poll(scheduler, descriptor, poll_mask) } -> bexec::sender;
    };

/**
 * Concept satisfied when a scheduler returns a sender from async_resolve.
 */
template <class Scheduler, class Query>
concept resolves_dns =
    bexec::scheduler<std::remove_cvref_t<Scheduler>> &&
    requires(Scheduler& scheduler, Query&& query,
             async_io::dns_result_view result) {
      {
        async_resolve(scheduler, std::forward<Query>(query), result)
      } -> bexec::sender;
    };

}  // namespace bupp

#endif  // BUPP_IO_CONTEXT_CPO_H_
