/**
 * @file resolve.h
 * @brief DNS resolution sender.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_DNS_RESOLVE_H_
#define BNIO_ASYNC_IO_DNS_RESOLVE_H_

#include <bnio/async_io/dns/query.h>
#include <bnio/async_io/dns/result.h>
#include <bnio/export.h>

#include <cstddef>
#include <system_error>

namespace bnio::async_io {

/**
 * Error category for platform resolver failures.
 */
[[nodiscard]] BNIO_EXPORT const std::error_category& dns_category() noexcept;

/**
 * Converts a platform resolver result code into a std::error_code.
 */
[[nodiscard]] BNIO_EXPORT std::error_code make_dns_error_code(
    int result) noexcept;

/** @cond BNIO_DETAIL */
namespace detail {

[[nodiscard]] BNIO_EXPORT std::error_code resolve_dns_platform(
    dns_query_view query, dns_result_view result, std::size_t& count) noexcept;

}  // namespace detail
/** @endcond */

/**
 * Resolves a DNS query synchronously into caller-provided endpoint storage.
 */
template <std::size_t HostCapacity, std::size_t ServiceCapacity>
[[nodiscard]] std::error_code resolve_dns(
    const basic_dns_query<HostCapacity, ServiceCapacity>& query,
    dns_result_view result, std::size_t& count) noexcept {
  count = 0;
  if (!result.valid()) {
    return std::make_error_code(std::errc::invalid_argument);
  }

  return detail::resolve_dns_platform(query.view(), result, count);
}

}  // namespace bnio::async_io

#endif  // BNIO_ASYNC_IO_DNS_RESOLVE_H_
