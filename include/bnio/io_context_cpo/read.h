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

/** Customization point object for async_read. */
struct async_read_t {
  /**
   * Streaming form: the stream's async_read if present, else the provider's;
   * the source advances its position (kernel position for descriptor_view).
   * Unsupported combinations (e.g. random_access_file without an offset) are
   * removed from overload resolution, not rejected in the body.
   */
  template <class Provider, class Source, class Buffer>
    requires(
        requires {
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

  /** Mode form: @p mode is stream flags if handled, else a provider offset. */
  template <class Provider, class Source, class Buffer, class Mode>
    requires(
        requires {
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

/** Customization point object for async_read_some. */
struct async_read_some_t {
  /** Streaming form: the stream's async_read_some, else the provider's. */
  template <class Provider, class Source, class Buffer>
    requires(
        requires {
          std::declval<Source>().async_read_some(std::declval<Provider>(),
                                                 std::declval<Buffer>());
        } ||
        requires {
          std::declval<Provider>().async_read_some(std::declval<Source>(),
                                                   std::declval<Buffer>());
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

  /** Mode form: @p mode is stream flags if handled, else a provider offset. */
  template <class Provider, class Source, class Buffer, class Mode>
    requires(
        requires {
          std::declval<Source>().async_read_some(std::declval<Provider>(),
                                                 std::declval<Buffer>(),
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
