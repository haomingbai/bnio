#pragma once
#ifndef BUPP_IO_CONTEXT_CPO_WRITE_H_
#define BUPP_IO_CONTEXT_CPO_WRITE_H_

#include <utility>

namespace bupp {

/**
 * Customization point object for a single asynchronous write operation.
 */
struct async_write_t {
  /**
   * Invokes async_write on a stream when available, otherwise on a provider.
   */
  template <class Provider, class Sink, class Buffer, class Mode = int>
  constexpr decltype(auto) operator()(Provider&& provider, Sink&& sink,
                                      Buffer&& buffer, Mode mode = 0) const {
    if constexpr (requires {
                    std::forward<Sink>(sink).async_write(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer), mode);
                  }) {
      return std::forward<Sink>(sink).async_write(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer), mode);
    } else {
      return std::forward<Provider>(provider).async_write(
          std::forward<Sink>(sink), std::forward<Buffer>(buffer), mode);
    }
  }
};

/**
 * Customization point object for one asynchronous write operation.
 */
struct async_write_some_t {
  /**
   * Invokes async_write_some on a stream when available, otherwise on a
   * provider.
   */
  template <class Provider, class Sink, class Buffer, class Mode = int>
  constexpr decltype(auto) operator()(Provider&& provider, Sink&& sink,
                                      Buffer&& buffer, Mode mode = 0) const {
    if constexpr (requires {
                    std::forward<Sink>(sink).async_write_some(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer), mode);
                  }) {
      return std::forward<Sink>(sink).async_write_some(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer), mode);
    } else {
      return std::forward<Provider>(provider).async_write_some(
          std::forward<Sink>(sink), std::forward<Buffer>(buffer), mode);
    }
  }
};

/**
 * Customization point object for an asynchronous write that bypasses the
 * scheduler's queued I/O batch and submits immediately.
 */
struct async_write_direct_t {
  /**
   * Invokes async_write_direct on a stream when that direct-submission
   * customization exists, otherwise on a provider.
   */
  template <class Provider, class Sink, class Buffer, class Mode = int>
  constexpr decltype(auto) operator()(Provider&& provider, Sink&& sink,
                                      Buffer&& buffer, Mode mode = 0) const {
    if constexpr (requires {
                    std::forward<Sink>(sink).async_write_direct(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer), mode);
                  }) {
      return std::forward<Sink>(sink).async_write_direct(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer), mode);
    } else {
      return std::forward<Provider>(provider).async_write_direct(
          std::forward<Sink>(sink), std::forward<Buffer>(buffer), mode);
    }
  }
};

/**
 * Customization point object for one direct-submission asynchronous write
 * operation.
 */
struct async_write_some_direct_t {
  /**
   * Invokes async_write_some_direct on a stream when available, otherwise on a
   * provider.
   */
  template <class Provider, class Sink, class Buffer, class Mode = int>
  constexpr decltype(auto) operator()(Provider&& provider, Sink&& sink,
                                      Buffer&& buffer, Mode mode = 0) const {
    if constexpr (requires {
                    std::forward<Sink>(sink).async_write_some_direct(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer), mode);
                  }) {
      return std::forward<Sink>(sink).async_write_some_direct(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer), mode);
    } else {
      return std::forward<Provider>(provider).async_write_some_direct(
          std::forward<Sink>(sink), std::forward<Buffer>(buffer), mode);
    }
  }
};

}  // namespace bupp

#endif  // BUPP_IO_CONTEXT_CPO_WRITE_H_
