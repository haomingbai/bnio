/**
 * @file read.h
 * @brief Read CPO (async_read, async_read_some).
 */

#pragma once
#ifndef BNIO_IO_CONTEXT_CPO_READ_H_
#define BNIO_IO_CONTEXT_CPO_READ_H_

#include <utility>

namespace bnio {

/**
 * Customization point object for a single asynchronous read operation.
 */
struct async_read_t {
  /**
   * Invokes async_read on a stream when available, otherwise on a provider.
   */
  template <class Provider, class Source, class Buffer, class Mode = int>
  constexpr decltype(auto) operator()(Provider&& provider, Source&& source,
                                      Buffer&& buffer, Mode mode = 0) const {
    if constexpr (requires {
                    std::forward<Source>(source).async_read(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer), mode);
                  }) {
      return std::forward<Source>(source).async_read(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer), mode);
    } else {
      return std::forward<Provider>(provider).async_read(
          std::forward<Source>(source), std::forward<Buffer>(buffer), mode);
    }
  }
};

/**
 * Customization point object for one asynchronous read operation.
 */
struct async_read_some_t {
  /**
   * Invokes async_read_some on a stream when available, otherwise on a
   * provider.
   */
  template <class Provider, class Source, class Buffer, class Mode = int>
  constexpr decltype(auto) operator()(Provider&& provider, Source&& source,
                                      Buffer&& buffer, Mode mode = 0) const {
    if constexpr (requires {
                    std::forward<Source>(source).async_read_some(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer), mode);
                  }) {
      return std::forward<Source>(source).async_read_some(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer), mode);
    } else {
      return std::forward<Provider>(provider).async_read_some(
          std::forward<Source>(source), std::forward<Buffer>(buffer), mode);
    }
  }
};

}  // namespace bnio

#endif  // BNIO_IO_CONTEXT_CPO_READ_H_
