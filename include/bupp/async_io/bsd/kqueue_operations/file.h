#pragma once
#ifndef BUPP_ASYNC_IO_BSD_KQUEUE_OPERATIONS_FILE_H_
#define BUPP_ASYNC_IO_BSD_KQUEUE_OPERATIONS_FILE_H_

#include <bupp/async_io/bsd/detail/kqueue_receiver_operation.h>
#include <bupp/async_io/bsd/kqueue_operations/detail/io_request.h>
#include <bupp/async_io/buffer_view.h>
#include <bupp/async_io/descriptor_view.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <limits>
#include <utility>

namespace bupp::async_io::bsd_native {

namespace detail {

[[nodiscard]] inline std::size_t bounded_file_io_size(
    std::size_t size) noexcept {
  return std::min(size, static_cast<std::size_t>(INT_MAX));
}

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

[[nodiscard]] inline int set_descriptor_nonblocking(int descriptor) noexcept {
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0) {
    return -errno;
  }
  if ((flags & O_NONBLOCK) != 0) {
    return 0;
  }
  return ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -errno;
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

/** A descriptor read that selects file or readiness behavior in start(). */
class kqueue_file_read_request {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  kqueue_file_read_request(descriptor_view descriptor, buffer_view buffer,
                           std::uint64_t offset) noexcept
      : descriptor_(descriptor), buffer_(buffer), offset_(offset) {}

  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_read(descriptor_.native_handle());
  }

  [[nodiscard]] int start_io() noexcept {
    if (buffer_.size > 0 && buffer_.data == nullptr) {
      return -EFAULT;
    }
    struct stat status {};
    if (::fstat(descriptor_.native_handle(), &status) != 0) {
      return -errno;
    }
    regular_file_ = S_ISREG(status.st_mode);
    if (regular_file_) {
      return perform_positioned_io();
    }
    const int nonblocking =
        detail::set_descriptor_nonblocking(descriptor_.native_handle());
    return nonblocking < 0 ? nonblocking : perform_io();
  }

  [[nodiscard]] int perform_io() noexcept {
    if (regular_file_) {
      return perform_positioned_io();
    }
    return detail::nonblocking_descriptor_result(
        ::read(descriptor_.native_handle(), buffer_.data,
               detail::bounded_file_io_size(buffer_.size)));
  }

  [[nodiscard]] bool should_wait(int result) const noexcept {
    return !regular_file_ && detail::should_wait(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<std::size_t>(result));
  }

 private:
  [[nodiscard]] int perform_positioned_io() noexcept {
    if (!detail::valid_file_offset(offset_)) {
      return -EOVERFLOW;
    }

    ssize_t result;
    do {
      result = ::pread(descriptor_.native_handle(), buffer_.data,
                       detail::bounded_file_io_size(buffer_.size),
                       static_cast<off_t>(offset_));
    } while (result < 0 && errno == EINTR);
    return detail::positioned_io_result(result);
  }

  descriptor_view descriptor_;
  buffer_view buffer_;
  std::uint64_t offset_;
  bool regular_file_ = false;
};

/** A descriptor write that selects file or readiness behavior in start(). */
class kqueue_file_write_request {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  kqueue_file_write_request(descriptor_view descriptor, const void* data,
                            std::size_t size, std::uint64_t offset) noexcept
      : descriptor_(descriptor), data_(data), size_(size), offset_(offset) {}

  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_write(descriptor_.native_handle());
  }

  [[nodiscard]] int start_io() noexcept {
    if (size_ > 0 && data_ == nullptr) {
      return -EFAULT;
    }
    struct stat status {};
    if (::fstat(descriptor_.native_handle(), &status) != 0) {
      return -errno;
    }
    regular_file_ = S_ISREG(status.st_mode);
    if (regular_file_) {
      return perform_positioned_io();
    }
    const int nonblocking =
        detail::set_descriptor_nonblocking(descriptor_.native_handle());
    return nonblocking < 0 ? nonblocking : perform_io();
  }

  [[nodiscard]] int perform_io() noexcept {
    if (regular_file_) {
      return perform_positioned_io();
    }
    return detail::nonblocking_descriptor_result(
        ::write(descriptor_.native_handle(), data_,
                detail::bounded_file_io_size(size_)));
  }

  [[nodiscard]] bool should_wait(int result) const noexcept {
    return !regular_file_ && detail::should_wait(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<std::size_t>(result));
  }

 private:
  [[nodiscard]] int perform_positioned_io() noexcept {
    if (!detail::valid_file_offset(offset_)) {
      return -EOVERFLOW;
    }

    ssize_t result;
    do {
      result = ::pwrite(descriptor_.native_handle(), data_,
                        detail::bounded_file_io_size(size_),
                        static_cast<off_t>(offset_));
    } while (result < 0 && errno == EINTR);
    return detail::positioned_io_result(result);
  }

  descriptor_view descriptor_;
  const void* data_;
  std::size_t size_;
  std::uint64_t offset_;
  bool regular_file_ = false;
};

using kqueue_file_read_sender =
    detail::kqueue_ready_io_sender<kqueue_file_read_request>;
using kqueue_file_write_sender =
    detail::kqueue_ready_io_sender<kqueue_file_write_request>;

/** @cond BUPP_DETAIL */

inline auto kqueue_context::async_read(descriptor_view descriptor,
                                       buffer_view buffer,
                                       std::uint64_t offset) {
  return kqueue_file_read_sender(
      *this, kqueue_file_read_request(descriptor, buffer, offset));
}

inline auto kqueue_context::async_write(descriptor_view descriptor,
                                        const void* data, std::size_t size,
                                        std::uint64_t offset) {
  return kqueue_file_write_sender(
      *this, kqueue_file_write_request(descriptor, data, size, offset));
}

/** @endcond */

class kqueue_raw_read_request : public kqueue_file_read_request {
 public:
  using kqueue_file_read_request::kqueue_file_read_request;

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned flags) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), result, flags);
  }
};

class kqueue_raw_write_request : public kqueue_file_write_request {
 public:
  kqueue_raw_write_request(descriptor_view descriptor, buffer_view buffer)
      : kqueue_file_write_request(descriptor, buffer.data, buffer.size, 0) {}

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned flags) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), result, flags);
  }
};

/** Low-level read operation backed by an objectized descriptor request. */
template <class Receiver>
class kqueue_read_operation
    : public detail::kqueue_ready_io_operation<kqueue_raw_read_request,
                                               Receiver> {
 public:
  kqueue_read_operation(kqueue_context& context, descriptor_view descriptor,
                        buffer_view buffer, Receiver receiver)
      : detail::kqueue_ready_io_operation<kqueue_raw_read_request, Receiver>(
            context, kqueue_raw_read_request(descriptor, buffer, 0),
            std::move(receiver)) {}
};

/** Low-level write operation backed by an objectized descriptor request. */
template <class Receiver>
class kqueue_write_operation
    : public detail::kqueue_ready_io_operation<kqueue_raw_write_request,
                                               Receiver> {
 public:
  kqueue_write_operation(kqueue_context& context, descriptor_view descriptor,
                         buffer_view buffer, Receiver receiver)
      : detail::kqueue_ready_io_operation<kqueue_raw_write_request, Receiver>(
            context, kqueue_raw_write_request(descriptor, buffer),
            std::move(receiver)) {}
};

}  // namespace bupp::async_io::bsd_native

#endif  // BUPP_ASYNC_IO_BSD_KQUEUE_OPERATIONS_FILE_H_
