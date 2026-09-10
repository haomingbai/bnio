/**
 * @file stream_file_io.h
 * @brief Streaming descriptor file I/O operation sender factories.
 */

#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_STREAM_FILE_IO_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_POSIX_IO_CONTEXT_STREAM_FILE_IO_H_

namespace bnio {

template <io_context::schedule_kind Kind>
inline auto io_context::async_read(async_io::descriptor_view descriptor,
                                   mutable_buffer buffer) {
  return detail::write_all_sender<detail::stream_file_read_all_state<Kind> >(
      detail::stream_file_read_all_state<Kind>(*this, descriptor, buffer));
}

template <io_context::schedule_kind Kind>
inline auto io_context::async_read_some(async_io::descriptor_view descriptor,
                                        mutable_buffer buffer) {
  return detail::make_io_sender<Kind>(
      *this, detail::make_stream_file_read_request(descriptor, buffer));
}

template <io_context::schedule_kind Kind>
inline auto io_context::async_write(async_io::descriptor_view descriptor,
                                    const_buffer buffer) {
  return detail::write_all_sender<detail::stream_file_write_all_state<Kind> >(
      detail::stream_file_write_all_state<Kind>(*this, descriptor, buffer));
}

template <io_context::schedule_kind Kind>
inline auto io_context::async_write_some(async_io::descriptor_view descriptor,
                                         const_buffer buffer) {
  return detail::make_io_sender<Kind>(
      *this, detail::make_stream_file_write_request(descriptor, buffer));
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read(
    async_io::descriptor_view descriptor, mutable_buffer buffer) const {
  return context_->async_read<Kind>(descriptor, buffer);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_read_some(
    async_io::descriptor_view descriptor, mutable_buffer buffer) const {
  return context_->async_read_some<Kind>(descriptor, buffer);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write(
    async_io::descriptor_view descriptor, const_buffer buffer) const {
  return context_->async_write<Kind>(descriptor, buffer);
}

template <io_context::schedule_kind Kind>
auto io_context::basic_scheduler<Kind>::async_write_some(
    async_io::descriptor_view descriptor, const_buffer buffer) const {
  return context_->async_write_some<Kind>(descriptor, buffer);
}

}  // namespace bnio

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_STREAM_FILE_IO_H_
