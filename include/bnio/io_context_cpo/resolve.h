/**
 * @file resolve.h
 * @brief Resolve CPO (async_resolve).
 */

#pragma once
#ifndef BNIO_IO_CONTEXT_CPO_RESOLVE_H_
#define BNIO_IO_CONTEXT_CPO_RESOLVE_H_

#include <utility>

namespace bnio {

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

}  // namespace bnio

#endif  // BNIO_IO_CONTEXT_CPO_RESOLVE_H_
