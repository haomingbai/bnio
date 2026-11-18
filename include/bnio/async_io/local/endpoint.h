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
 * Local (AF_UNIX) endpoint value type: inline path buffer, trivially copyable.
 *
 * Sized after sockaddr_un::sun_path; an oversized path degrades to
 * unspecified, which consuming calls report as invalid_argument (no syscall).
 */
class BNIO_EXPORT endpoint {
 public:
  /**
   * Path capacity, matching sockaddr_un::sun_path (108 Linux, 104 BSD).
   */
  static constexpr std::size_t max_path_length = sizeof(sockaddr_un::sun_path);

  /**
   * Creates an unspecified endpoint; also the form of unnamed peers.
   */
  endpoint() noexcept;

  /**
   * Creates a path-name endpoint; unspecified if @p path does not fit.
   */
  explicit endpoint(std::string_view path) noexcept;

#if defined(BNIO_HAS_ASYNC_IO_LINUX)
  /**
   * Creates an abstract-namespace endpoint; Linux-only. The leading NUL is not
   * part of @p name, and an oversized name leaves the endpoint unspecified.
   */
  static endpoint abstract(std::string_view name) noexcept;
#endif

  /**
   * Returns the address form carried by this endpoint.
   */
  [[nodiscard]] endpoint_kind kind() const noexcept;

  /**
   * Path or abstract name without the leading NUL; empty if unspecified.
   */
  [[nodiscard]] std::string_view path() const noexcept;

 private:
  endpoint_kind kind_ = endpoint_kind::unspecified;
  std::size_t length_ = 0;
  char path_[max_path_length] = {};
};

}  // namespace bnio::async_io::local

#endif  // BNIO_ASYNC_IO_LOCAL_ENDPOINT_H_
