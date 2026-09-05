/**
 * @file file_factories.h
 * @brief BSD native file I/O sender factories.
 */

#ifndef BNIO_DETAIL_BSD_IO_CONTEXT_NATIVE_IO_FILE_FACTORIES_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_BSD_IO_CONTEXT_NATIVE_IO_FILE_FACTORIES_H_

#include <bnio/async_io/random_access_file.h>
#include <bnio/detail/bsd/io_context_native_io/common.h>

namespace bnio::detail {

[[nodiscard]] inline auto make_stream_file_read_request(
    async_io::descriptor_view descriptor, mutable_buffer buffer) {
  return async_io::bsd_native::kqueue_stream_file_read_request(descriptor,
                                                               buffer.view());
}

[[nodiscard]] inline auto make_stream_file_write_request(
    async_io::descriptor_view descriptor, const_buffer buffer) {
  return async_io::bsd_native::kqueue_stream_file_write_request(
      descriptor, buffer.data(), buffer.size());
}

[[nodiscard]] inline auto make_random_access_read_request(
    async_io::random_access_file file, mutable_buffer buffer,
    std::uint64_t offset) {
  return async_io::bsd_native::kqueue_random_access_read_request(
      file, buffer.view(), offset);
}

[[nodiscard]] inline auto make_random_access_write_request(
    async_io::random_access_file file, const_buffer buffer,
    std::uint64_t offset) {
  return async_io::bsd_native::kqueue_random_access_write_request(
      file, buffer.data(), buffer.size(), offset);
}

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_BSD_IO_CONTEXT_NATIVE_IO_FILE_FACTORIES_H_
