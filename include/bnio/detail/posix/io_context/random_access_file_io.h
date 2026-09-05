/**
 * @file random_access_file_io.h
 * @brief Random access file I/O operation sender factories.
 */

#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_RANDOM_ACCESS_FILE_IO_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_POSIX_IO_CONTEXT_RANDOM_ACCESS_FILE_IO_H_

namespace bnio {

inline auto io_context::async_read(async_io::random_access_file file,
                                   mutable_buffer buffer,
                                   std::uint64_t offset) {
  return detail::write_all_sender<detail::random_access_read_all_state>(
      detail::random_access_read_all_state(*this, file, buffer, offset));
}

inline auto io_context::async_read_some(async_io::random_access_file file,
                                        mutable_buffer buffer,
                                        std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::make_random_access_read_request(file, buffer, offset));
}

inline auto io_context::async_write(async_io::random_access_file file,
                                    const_buffer buffer, std::uint64_t offset) {
  return detail::write_all_sender(
      detail::random_access_write_all_state(*this, file, buffer, offset));
}

inline auto io_context::async_write_some(async_io::random_access_file file,
                                         const_buffer buffer,
                                         std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::make_random_access_write_request(file, buffer, offset));
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read(
    async_io::random_access_file file, mutable_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_read(file, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read_some(
    async_io::random_access_file file, mutable_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_read_some(file, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write(
    async_io::random_access_file file, const_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_write(file, buffer, offset);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write_some(
    async_io::random_access_file file, const_buffer buffer,
    std::uint64_t offset) const {
  return context_->async_write_some(file, buffer, offset);
}

}  // namespace bnio

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_RANDOM_ACCESS_FILE_IO_H_
