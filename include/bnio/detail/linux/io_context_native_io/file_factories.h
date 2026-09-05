/**
 * @file file_factories.h
 * @brief Linux native file I/O sender factories.
 */

#ifndef BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_FILE_FACTORIES_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_FILE_FACTORIES_H_

#include <bnio/async_io/random_access_file.h>
#include <bnio/detail/linux/io_context_native_io/stream_file.h>
#include <bnio/detail/linux/io_context_native_io/random_access_file.h>

namespace bnio::detail {

[[nodiscard]] inline auto make_stream_file_read_request(
    async_io::descriptor_view descriptor, mutable_buffer buffer) {
  return stream_file_read_model(descriptor, buffer);
}

[[nodiscard]] inline auto make_stream_file_write_request(
    async_io::descriptor_view descriptor, const_buffer buffer) {
  return stream_file_write_model(descriptor, buffer);
}

[[nodiscard]] inline auto make_random_access_read_request(
    async_io::random_access_file file, mutable_buffer buffer,
    std::uint64_t offset) {
  return random_access_read_model(file, buffer, offset);
}

[[nodiscard]] inline auto make_random_access_write_request(
    async_io::random_access_file file, const_buffer buffer,
    std::uint64_t offset) {
  return random_access_write_model(file, buffer, offset);
}

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_FILE_FACTORIES_H_
