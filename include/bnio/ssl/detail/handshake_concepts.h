/**
 * @file handshake_concepts.h
 * @brief Concepts for the factory-driven SSL handshake retry surface.
 */

#pragma once
#ifndef BNIO_SSL_DETAIL_HANDSHAKE_CONCEPTS_H_
#define BNIO_SSL_DETAIL_HANDSHAKE_CONCEPTS_H_

#include <bexec/completion_signatures.hpp>
#include <bexec/env.hpp>
#include <bexec/receiver.hpp>
#include <bexec/sender.hpp>
#include <concepts>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::ssl {

/** @cond BNIO_DETAIL */
namespace detail {

/**
 * Minimal receiver accepting set_value(std::error_code) and set_stopped();
 * also used to check a child sender.
 */
struct handshake_retry_probe {
  void set_value(std::error_code) noexcept;
  void set_stopped() noexcept;
};

/**
 * Accepts senders completing through the retry contract: valid in Env, able
 * to receive the probe, with exactly one value signature
 * set_value(std::error_code) and no set_error channel. set_stopped() is
 * accepted but not required.
 */
template <class Sender, class Env = bexec::empty_env>
concept handshake_retry_step_sender =
    bexec::sender_in<Sender, Env> &&
    bexec::receiver_of<handshake_retry_probe,
                       bexec::completion_signatures_of_t<Sender, Env>> &&
    (bexec::completion_signatures_of_t<Sender, Env>::template count_of<
         bexec::set_value_t>() == 1);

/**
 * Accepts factories performing the user wait work for a callback-driven
 * pause: stored by value, invoked as an lvalue with a const std::error_code&,
 * returning a handshake_retry_step_sender by value.
 */
template <class Factory>
concept handshake_retry_factory =
    std::move_constructible<std::decay_t<Factory>> &&
    std::invocable<std::decay_t<Factory>&, const std::error_code&> &&
    std::same_as<
        std::invoke_result_t<std::decay_t<Factory>&, const std::error_code&>,
        std::remove_cvref_t<std::invoke_result_t<std::decay_t<Factory>&,
                                                 const std::error_code&>>> &&
    handshake_retry_step_sender<
        std::invoke_result_t<std::decay_t<Factory>&, const std::error_code&>>;

}  // namespace detail
/** @endcond */

}  // namespace bnio::ssl

#endif  // BNIO_SSL_DETAIL_HANDSHAKE_CONCEPTS_H_
