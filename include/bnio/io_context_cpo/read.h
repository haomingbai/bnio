/**
 * @file read.h
 * @brief Read CPO (async_read, async_read_some).
 */

#pragma once
#ifndef BNIO_IO_CONTEXT_CPO_READ_H_
#define BNIO_IO_CONTEXT_CPO_READ_H_

#include <cstdint>
#include <utility>

namespace bnio {

/**
 * Customization point object for a single asynchronous read operation.
 */
struct async_read_t {
  /**
   * Streaming form: invokes async_read on a stream when available, otherwise
   * on a provider. The source/provider advances its position naturally
   * (kernel file position for descriptor_view).
   *
   * Constrained on branch viability so that an unsupported combination (e.g.
   * a random_access_file without an offset) is removed from overload
   * resolution instead of failing inside the body.
   */
  template <class Provider, class Source, class Buffer>
    requires(requires {
               std::declval<Source>().async_read(std::declval<Provider>(),
                                                 std::declval<Buffer>());
             } ||
             requires {
               std::declval<Provider>().async_read(std::declval<Source>(),
                                                   std::declval<Buffer>());
             })
  constexpr decltype(auto) operator()(Provider&& provider, Source&& source,
                                      Buffer&& buffer) const {
    if constexpr (requires {
                    std::forward<Source>(source).async_read(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer));
                  }) {
      return std::forward<Source>(source).async_read(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer));
    } else {
      return std::forward<Provider>(provider).async_read(
          std::forward<Source>(source), std::forward<Buffer>(buffer));
    }
  }

  /**
   * Mode form: passes mode through to a stream (e.g. socket flags) when
   * available, otherwise to a positioned provider as an explicit offset
   * (random_access_file).
   */
  template <class Provider, class Source, class Buffer, class Mode>
    requires(requires {
               std::declval<Source>().async_read(std::declval<Provider>(),
                                                 std::declval<Buffer>(),
                                                 std::declval<Mode>());
             } ||
             requires {
               std::declval<Provider>().async_read(
                   std::declval<Source>(), std::declval<Buffer>(),
                   static_cast<std::uint64_t>(std::declval<Mode>()));
             })
  constexpr decltype(auto) operator()(Provider&& provider, Source&& source,
                                      Buffer&& buffer, Mode mode) const {
    if constexpr (requires {
                    std::forward<Source>(source).async_read(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer), mode);
                  }) {
      return std::forward<Source>(source).async_read(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer), mode);
    } else {
      return std::forward<Provider>(provider).async_read(
          std::forward<Source>(source), std::forward<Buffer>(buffer),
          static_cast<std::uint64_t>(mode));
    }
  }
};

/**
 * Customization point object for one asynchronous read operation.
 */
struct async_read_some_t {
  /**
   * Streaming form: invokes async_read_some on a stream when available,
   * otherwise on a provider.
   */
  template <class Provider, class Source, class Buffer>
    requires(requires {
               std::declval<Source>().async_read_some(
                   std::declval<Provider>(), std::declval<Buffer>());
             } ||
             requires {
               std::declval<Provider>().async_read_some(
                   std::declval<Source>(), std::declval<Buffer>());
             })
  constexpr decltype(auto) operator()(Provider&& provider, Source&& source,
                                      Buffer&& buffer) const {
    if constexpr (requires {
                    std::forward<Source>(source).async_read_some(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer));
                  }) {
      return std::forward<Source>(source).async_read_some(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer));
    } else {
      return std::forward<Provider>(provider).async_read_some(
          std::forward<Source>(source), std::forward<Buffer>(buffer));
    }
  }

  /**
   * Mode form: passes mode through to a stream (e.g. socket flags) when
   * available, otherwise to a positioned provider as an explicit offset
   * (random_access_file).
   */
  template <class Provider, class Source, class Buffer, class Mode>
    requires(requires {
               std::declval<Source>().async_read_some(
                   std::declval<Provider>(), std::declval<Buffer>(),
                   std::declval<Mode>());
             } ||
             requires {
               std::declval<Provider>().async_read_some(
                   std::declval<Source>(), std::declval<Buffer>(),
                   static_cast<std::uint64_t>(std::declval<Mode>()));
             })
  constexpr decltype(auto) operator()(Provider&& provider, Source&& source,
                                      Buffer&& buffer, Mode mode) const {
    if constexpr (requires {
                    std::forward<Source>(source).async_read_some(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer), mode);
                  }) {
      return std::forward<Source>(source).async_read_some(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer), mode);
    } else {
      return std::forward<Provider>(provider).async_read_some(
          std::forward<Source>(source), std::forward<Buffer>(buffer),
          static_cast<std::uint64_t>(mode));
    }
  }
};

}  // namespace bnio

#endif  // BNIO_IO_CONTEXT_CPO_READ_H_
