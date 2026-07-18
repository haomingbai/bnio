#pragma once
#ifndef BNIO_IO_CONTEXT_CPO_WRITE_H_
#define BNIO_IO_CONTEXT_CPO_WRITE_H_

#include <utility>

namespace bnio {

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

}  // namespace bnio

#endif  // BNIO_IO_CONTEXT_CPO_WRITE_H_
