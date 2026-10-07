/**
 * @file endpoint.h
 * @brief Local (AF_UNIX) endpoint value type.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LOCAL_ENDPOINT_H_
#define BNIO_ASYNC_IO_LOCAL_ENDPOINT_H_

#include <bnio/async_io/config.h>
#include <bnio/export.h>
#include <sys/un.h>

#include <cstddef>
#include <string_view>

namespace bnio::async_io::local {

/**
 * Address form carried by a local endpoint.
 */
enum class endpoint_kind {
  /**
   * No address is set. This is also the form decoded from unnamed peers.
   */
  unspecified,

  /**
   * Filesystem path address.
   */
  path_name,

  /**
   * Abstract-namespace address (Linux only).
   */
  abstract,
};

/**
 * Value type representing a local (AF_UNIX) endpoint.
 *
 * Endpoints own their path in a fixed-capacity buffer sized after
 * sockaddr_un::sun_path; they are allocation-free and trivially copyable,
 * consistent with ip::address and ip::endpoint. A path that does not fit
 * the buffer cannot be represented, so the endpoint degrades to the
 * unspecified kind; socket calls consuming such an endpoint report
 * std::errc::invalid_argument from the sockaddr_un conversion before any
 * syscall.
 */
class BNIO_EXPORT endpoint {
 public:
  /**
   * Maximum path byte capacity, matching sockaddr_un::sun_path (108 on
   * Linux, 104 on BSD).
   */
  static constexpr std::size_t max_path_length = sizeof(sockaddr_un::sun_path);

  /**
   * Creates an unspecified endpoint. This is also the form of unnamed
   * peers reported by endpoint queries.
   */
  endpoint() noexcept;

  /**
   * Creates a path-name endpoint. When @p path does not fit the fixed
   * buffer, the endpoint is left unspecified.
   */
  explicit endpoint(std::string_view path) noexcept;

#if defined(BNIO_HAS_ASYNC_IO_LINUX)
  /**
   * Creates an abstract-namespace endpoint; the leading NUL of the native
   * address is not part of @p name. When @p name does not fit, the
   * endpoint is left unspecified.
   *
   * The abstract namespace is a Linux convention, so this factory exists
   * only where the Linux backend is compiled in.
   */
  static endpoint abstract(std::string_view name) noexcept;
#endif

  /**
   * Returns the address form carried by this endpoint.
   */
  [[nodiscard]] endpoint_kind kind() const noexcept;

  /**
   * Returns the endpoint path or abstract name without the leading NUL,
   * or an empty view for an unspecified endpoint.
   */
  [[nodiscard]] std::string_view path() const noexcept;

 private:
  endpoint_kind kind_ = endpoint_kind::unspecified;
  std::size_t length_ = 0;
  char path_[max_path_length] = {};
};

}  // namespace bnio::async_io::local

#endif  // BNIO_ASYNC_IO_LOCAL_ENDPOINT_H_
