/**
 * @file write.h
 * @brief Write CPO (async_write, async_write_some).
 */

#pragma once
#ifndef BNIO_IO_CONTEXT_CPO_WRITE_H_
#define BNIO_IO_CONTEXT_CPO_WRITE_H_

#include <cstdint>
#include <utility>

namespace bnio {

/**
 * Customization point object for a single asynchronous write operation.
 */
struct async_write_t {
  /**
   * Streaming form: invokes async_write on a stream when available,
   * otherwise on a provider. The sink/provider advances its position
   * naturally (kernel file position for descriptor_view).
   *
   * Constrained on branch viability so that an unsupported combination (e.g.
   * a random_access_file without an offset) is removed from overload
   * resolution instead of failing inside the body.
   */
  template <class Provider, class Sink, class Buffer>
    requires(requires {
               std::declval<Sink>().async_write(std::declval<Provider>(),
                                                std::declval<Buffer>());
             } ||
             requires {
               std::declval<Provider>().async_write(std::declval<Sink>(),
                                                    std::declval<Buffer>());
             })
  constexpr decltype(auto) operator()(Provider&& provider, Sink&& sink,
                                      Buffer&& buffer) const {
    if constexpr (requires {
                    std::forward<Sink>(sink).async_write(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer));
                  }) {
      return std::forward<Sink>(sink).async_write(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer));
    } else {
      return std::forward<Provider>(provider).async_write(
          std::forward<Sink>(sink), std::forward<Buffer>(buffer));
    }
  }

  /**
   * Mode form: passes mode through to a stream (e.g. socket flags) when
   * available, otherwise to a positioned provider as an explicit offset
   * (random_access_file).
   */
  template <class Provider, class Sink, class Buffer, class Mode>
    requires(requires {
               std::declval<Sink>().async_write(std::declval<Provider>(),
                                                std::declval<Buffer>(),
                                                std::declval<Mode>());
             } ||
             requires {
               std::declval<Provider>().async_write(
                   std::declval<Sink>(), std::declval<Buffer>(),
                   static_cast<std::uint64_t>(std::declval<Mode>()));
             })
  constexpr decltype(auto) operator()(Provider&& provider, Sink&& sink,
                                      Buffer&& buffer, Mode mode) const {
    if constexpr (requires {
                    std::forward<Sink>(sink).async_write(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer), mode);
                  }) {
      return std::forward<Sink>(sink).async_write(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer), mode);
    } else {
      return std::forward<Provider>(provider).async_write(
          std::forward<Sink>(sink), std::forward<Buffer>(buffer),
          static_cast<std::uint64_t>(mode));
    }
  }
};

/**
 * Customization point object for one asynchronous write operation.
 */
struct async_write_some_t {
  /**
   * Streaming form: invokes async_write_some on a stream when available,
   * otherwise on a provider.
   */
  template <class Provider, class Sink, class Buffer>
    requires(requires {
               std::declval<Sink>().async_write_some(
                   std::declval<Provider>(), std::declval<Buffer>());
             } ||
             requires {
               std::declval<Provider>().async_write_some(
                   std::declval<Sink>(), std::declval<Buffer>());
             })
  constexpr decltype(auto) operator()(Provider&& provider, Sink&& sink,
                                      Buffer&& buffer) const {
    if constexpr (requires {
                    std::forward<Sink>(sink).async_write_some(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer));
                  }) {
      return std::forward<Sink>(sink).async_write_some(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer));
    } else {
      return std::forward<Provider>(provider).async_write_some(
          std::forward<Sink>(sink), std::forward<Buffer>(buffer));
    }
  }

  /**
   * Mode form: passes mode through to a stream (e.g. socket flags) when
   * available, otherwise to a positioned provider as an explicit offset
   * (random_access_file).
   */
  template <class Provider, class Sink, class Buffer, class Mode>
    requires(requires {
               std::declval<Sink>().async_write_some(
                   std::declval<Provider>(), std::declval<Buffer>(),
                   std::declval<Mode>());
             } ||
             requires {
               std::declval<Provider>().async_write_some(
                   std::declval<Sink>(), std::declval<Buffer>(),
                   static_cast<std::uint64_t>(std::declval<Mode>()));
             })
  constexpr decltype(auto) operator()(Provider&& provider, Sink&& sink,
                                      Buffer&& buffer, Mode mode) const {
    if constexpr (requires {
                    std::forward<Sink>(sink).async_write_some(
                        std::forward<Provider>(provider),
                        std::forward<Buffer>(buffer), mode);
                  }) {
      return std::forward<Sink>(sink).async_write_some(
          std::forward<Provider>(provider), std::forward<Buffer>(buffer), mode);
    } else {
      return std::forward<Provider>(provider).async_write_some(
          std::forward<Sink>(sink), std::forward<Buffer>(buffer),
          static_cast<std::uint64_t>(mode));
    }
  }
};

}  // namespace bnio

#endif  // BNIO_IO_CONTEXT_CPO_WRITE_H_
