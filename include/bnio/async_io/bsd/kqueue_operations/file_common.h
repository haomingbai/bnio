/**
 * @file file_common.h
 * @brief Shared helpers for kqueue file read/write operations.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_FILE_COMMON_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_FILE_COMMON_H_

#include <sys/types.h>

#include <cerrno>
#include <cstdint>
#include <limits>

namespace bnio::async_io::bsd_native {

namespace detail {

[[nodiscard]] inline int positioned_io_result(ssize_t result) noexcept {
  if (result >= 0) {
    return static_cast<int>(result);
  }
  return -errno;
}

[[nodiscard]] inline bool valid_file_offset(std::uint64_t offset) noexcept {
  return offset <=
         static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
}

[[nodiscard]] inline int nonblocking_descriptor_result(
    ssize_t result) noexcept {
  if (result >= 0) {
    return static_cast<int>(result);
  }
  const int error = errno;
  if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK) {
    return -EAGAIN;
  }
  return -error;
}

}  // namespace detail

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_FILE_COMMON_H_
