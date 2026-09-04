/**
 * @file file.h
 * @brief Linux native streaming and positioned file I/O models.
 */

#ifndef BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_FILE_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_FILE_H_

#include <bnio/async_io/random_access_file.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <system_error>

namespace bnio::detail {

/** SQE/preadv2 offset -1: use and advance the kernel file position. */
inline constexpr std::uint64_t streaming_file_offset =
    static_cast<std::uint64_t>(-1);

/** Positioned offsets must fit the kernel's signed loff_t. */
[[nodiscard]] inline bool valid_random_access_offset(
    std::uint64_t offset) noexcept {
  return offset <=
         static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

class descriptor_read_model {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  descriptor_read_model(async_io::descriptor_view descriptor,
                        mutable_buffer buffer) noexcept
      : descriptor_(descriptor), buffer_(buffer) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_read(
        descriptor_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        streaming_file_offset);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result = pread_nowait(
        descriptor_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        streaming_file_offset);
    if (result >= 0) {
      return static_cast<int>(result);
    }

    const int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK ||
        should_defer_nowait_error(error)) {
      return -EAGAIN;
    }
    return -error;
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  async_io::descriptor_view descriptor_;
  mutable_buffer buffer_;
};

class descriptor_write_model {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  descriptor_write_model(async_io::descriptor_view descriptor,
                         const_buffer buffer) noexcept
      : descriptor_(descriptor), buffer_(buffer) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_write(
        descriptor_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        streaming_file_offset);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result = pwrite_nowait(
        descriptor_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        streaming_file_offset);
    if (result >= 0) {
      return static_cast<int>(result);
    }

    const int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK ||
        should_defer_nowait_error(error)) {
      return -EAGAIN;
    }
    return -error;
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  async_io::descriptor_view descriptor_;
  const_buffer buffer_;
};

class random_access_read_model {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  random_access_read_model(async_io::random_access_file file,
                           mutable_buffer buffer,
                           std::uint64_t offset) noexcept
      : file_(file), buffer_(buffer), offset_(offset) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_read(
        file_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        offset_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    if (!valid_random_access_offset(offset_)) {
      return -EOVERFLOW;
    }

    const ssize_t result = pread_nowait(
        file_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        offset_);
    if (result >= 0) {
      return static_cast<int>(result);
    }

    const int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK ||
        should_defer_nowait_error(error)) {
      return -EAGAIN;
    }
    return -error;
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  async_io::random_access_file file_;
  mutable_buffer buffer_;
  std::uint64_t offset_;
};

class random_access_write_model {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  random_access_write_model(async_io::random_access_file file,
                            const_buffer buffer,
                            std::uint64_t offset) noexcept
      : file_(file), buffer_(buffer), offset_(offset) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_write(
        file_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        offset_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    if (!valid_random_access_offset(offset_)) {
      return -EOVERFLOW;
    }

    const ssize_t result = pwrite_nowait(
        file_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        offset_);
    if (result >= 0) {
      return static_cast<int>(result);
    }

    const int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK ||
        should_defer_nowait_error(error)) {
      return -EAGAIN;
    }
    return -error;
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  async_io::random_access_file file_;
  const_buffer buffer_;
  std::uint64_t offset_;
};

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_FILE_H_
