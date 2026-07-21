/**
 * @file poll.h
 * @brief Poll CPO (async_poll).
 */

#pragma once
#ifndef BNIO_IO_CONTEXT_CPO_POLL_H_
#define BNIO_IO_CONTEXT_CPO_POLL_H_

#include <utility>

namespace bnio {

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

}  // namespace bnio

#endif  // BNIO_IO_CONTEXT_CPO_POLL_H_
