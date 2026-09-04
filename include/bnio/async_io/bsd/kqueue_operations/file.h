/**
 * @file file.h
 * @brief kqueue streaming and positioned file read/write operations.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_FILE_H_
#define BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_FILE_H_

#include <bnio/async_io/bsd/kqueue_operations/detail/io_request.h>
#include <bnio/async_io/bsd/kqueue_operations/detail/native_io.h>
#include <bnio/async_io/buffer_view.h>
#include <bnio/async_io/descriptor_view.h>
#include <bnio/async_io/random_access_file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <utility>

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

/**
 * A streaming descriptor read. Regular files run ::read inline, advancing
 * the kernel file position; other descriptors are switched to O_NONBLOCK
 * and wait for kqueue readiness. The fstat classification is kept only to
 * select those two behaviors.
 */
class kqueue_descriptor_read_request {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  kqueue_descriptor_read_request(descriptor_view descriptor,
                                 buffer_view buffer) noexcept
      : descriptor_(descriptor), buffer_(buffer) {}

  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_read(descriptor_.native_handle());
  }

  [[nodiscard]] int start_io() noexcept {
    if (buffer_.size > 0 && buffer_.data == nullptr) {
      return -EFAULT;
    }
    return perform_io();
  }

  [[nodiscard]] int perform_io() noexcept {
    if (!resolved_) {
      struct stat status{};
      if (::fstat(descriptor_.native_handle(), &status) != 0) {
        return -errno;
      }
      regular_file_ = S_ISREG(status.st_mode);
      if (!regular_file_) {
        const int nonblocking =
            detail::set_descriptor_nonblocking(descriptor_.native_handle());
        if (nonblocking < 0) {
          return nonblocking;
        }
      }
      resolved_ = true;
    }
    if (regular_file_) {
      ssize_t result;
      do {
        result = ::read(descriptor_.native_handle(), buffer_.data,
                        detail::bounded_io_size(buffer_.size));
      } while (result < 0 && errno == EINTR);
      return detail::positioned_io_result(result);
    }
    return detail::nonblocking_descriptor_result(
        ::read(descriptor_.native_handle(), buffer_.data,
               detail::bounded_io_size(buffer_.size)));
  }

  [[nodiscard]] bool should_wait(int result) const noexcept {
    return !regular_file_ && detail::should_wait(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  descriptor_view descriptor_;
  buffer_view buffer_;
  bool regular_file_ = false;
  bool resolved_ = false;
};

/**
 * A streaming descriptor write. Regular files run ::write inline, advancing
 * the kernel file position; other descriptors are switched to O_NONBLOCK
 * and wait for kqueue readiness.
 */
class kqueue_descriptor_write_request {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  kqueue_descriptor_write_request(descriptor_view descriptor,
                                  const void* data, std::size_t size) noexcept
      : descriptor_(descriptor), data_(data), size_(size) {}

  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_write(descriptor_.native_handle());
  }

  [[nodiscard]] int start_io() noexcept {
    if (size_ > 0 && data_ == nullptr) {
      return -EFAULT;
    }
    return perform_io();
  }

  [[nodiscard]] int perform_io() noexcept {
    if (!resolved_) {
      struct stat status{};
      if (::fstat(descriptor_.native_handle(), &status) != 0) {
        return -errno;
      }
      regular_file_ = S_ISREG(status.st_mode);
      if (!regular_file_) {
        const int nonblocking =
            detail::set_descriptor_nonblocking(descriptor_.native_handle());
        if (nonblocking < 0) {
          return nonblocking;
        }
      }
      resolved_ = true;
    }
    if (regular_file_) {
      ssize_t result;
      do {
        result = ::write(descriptor_.native_handle(), data_,
                         detail::bounded_io_size(size_));
      } while (result < 0 && errno == EINTR);
      return detail::positioned_io_result(result);
    }
    return detail::nonblocking_descriptor_result(::write(
        descriptor_.native_handle(), data_, detail::bounded_io_size(size_)));
  }

  [[nodiscard]] bool should_wait(int result) const noexcept {
    return !regular_file_ && detail::should_wait(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  descriptor_view descriptor_;
  const void* data_;
  std::size_t size_;
  bool regular_file_ = false;
  bool resolved_ = false;
};

/**
 * A positioned read on a random access file: start performs ::pread at the
 * given offset without observing or advancing the kernel file position. The
 * caller guarantees a random access file, so no fstat dispatch happens and
 * the operation never waits on kqueue. Offsets beyond off_t are rejected
 * with EOVERFLOW before entering the kernel.
 */
class kqueue_random_access_read_request {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  kqueue_random_access_read_request(random_access_file file,
                                    buffer_view buffer,
                                    std::uint64_t offset) noexcept
      : file_(file), buffer_(buffer), offset_(offset) {}

  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_read(file_.native_handle());
  }

  [[nodiscard]] int start_io() noexcept {
    if (buffer_.size > 0 && buffer_.data == nullptr) {
      return -EFAULT;
    }
    return perform_io();
  }

  [[nodiscard]] int perform_io() noexcept {
    if (!detail::valid_file_offset(offset_)) {
      return -EOVERFLOW;
    }

    ssize_t result;
    do {
      result = ::pread(file_.native_handle(), buffer_.data,
                       detail::bounded_io_size(buffer_.size),
                       static_cast<off_t>(offset_));
    } while (result < 0 && errno == EINTR);
    return detail::positioned_io_result(result);
  }

  /** Positioned I/O is synchronous on a random access file: never wait. */
  [[nodiscard]] bool should_wait(int) const noexcept { return false; }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  random_access_file file_;
  buffer_view buffer_;
  std::uint64_t offset_;
};

/**
 * A positioned write on a random access file: start performs ::pwrite at
 * the given offset without observing or advancing the kernel file position.
 * Never waits on kqueue; offsets beyond off_t are rejected with EOVERFLOW
 * before entering the kernel.
 */
class kqueue_random_access_write_request {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  kqueue_random_access_write_request(random_access_file file,
                                     const void* data, std::size_t size,
                                     std::uint64_t offset) noexcept
      : file_(file), data_(data), size_(size), offset_(offset) {}

  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_write(file_.native_handle());
  }

  [[nodiscard]] int start_io() noexcept {
    if (size_ > 0 && data_ == nullptr) {
      return -EFAULT;
    }
    return perform_io();
  }

  [[nodiscard]] int perform_io() noexcept {
    if (!detail::valid_file_offset(offset_)) {
      return -EOVERFLOW;
    }

    ssize_t result;
    do {
      result =
          ::pwrite(file_.native_handle(), data_,
                   detail::bounded_io_size(size_), static_cast<off_t>(offset_));
    } while (result < 0 && errno == EINTR);
    return detail::positioned_io_result(result);
  }

  /** Positioned I/O is synchronous on a random access file: never wait. */
  [[nodiscard]] bool should_wait(int) const noexcept { return false; }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  random_access_file file_;
  const void* data_;
  std::size_t size_;
  std::uint64_t offset_;
};

using kqueue_descriptor_read_sender =
    detail::kqueue_ready_io_sender<kqueue_descriptor_read_request>;
using kqueue_descriptor_write_sender =
    detail::kqueue_ready_io_sender<kqueue_descriptor_write_request>;
using kqueue_random_access_read_sender =
    detail::kqueue_ready_io_sender<kqueue_random_access_read_request>;
using kqueue_random_access_write_sender =
    detail::kqueue_ready_io_sender<kqueue_random_access_write_request>;

/** @cond BNIO_DETAIL */

inline auto kqueue_context::async_read(descriptor_view descriptor,
                                       buffer_view buffer) {
  return kqueue_descriptor_read_sender(
      *this, kqueue_descriptor_read_request(descriptor, buffer));
}

inline auto kqueue_context::async_read(random_access_file file,
                                       buffer_view buffer,
                                       std::uint64_t offset) {
  return kqueue_random_access_read_sender(
      *this, kqueue_random_access_read_request(file, buffer, offset));
}

inline auto kqueue_context::async_write(descriptor_view descriptor,
                                        const void* data, std::size_t size) {
  return kqueue_descriptor_write_sender(
      *this, kqueue_descriptor_write_request(descriptor, data, size));
}

inline auto kqueue_context::async_write(random_access_file file,
                                        const void* data, std::size_t size,
                                        std::uint64_t offset) {
  return kqueue_random_access_write_sender(
      *this, kqueue_random_access_write_request(file, data, size, offset));
}

/** @endcond */

/** Streaming read that forwards the raw (ec, result, flags) completion. */
class kqueue_raw_descriptor_read_request
    : public kqueue_descriptor_read_request {
 public:
  using kqueue_descriptor_read_request::kqueue_descriptor_read_request;

  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code, int,
                                                      unsigned),
                                   bexec::set_stopped_t()>;

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned flags) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec, result, flags);
  }
};

/** Streaming write that forwards the raw (ec, result, flags) completion. */
class kqueue_raw_descriptor_write_request
    : public kqueue_descriptor_write_request {
 public:
  kqueue_raw_descriptor_write_request(descriptor_view descriptor,
                                      buffer_view buffer)
      : kqueue_descriptor_write_request(descriptor, buffer.data, buffer.size) {}

  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code, int,
                                                      unsigned),
                                   bexec::set_stopped_t()>;

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned flags) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec, result, flags);
  }
};

/** Low-level streaming read operation backed by an objectized request. */
template <class Receiver>
class kqueue_read_operation
    : public detail::kqueue_ready_io_operation<
          kqueue_raw_descriptor_read_request, Receiver> {
 public:
  kqueue_read_operation(kqueue_context& context, descriptor_view descriptor,
                        buffer_view buffer, Receiver receiver)
      : detail::kqueue_ready_io_operation<
            kqueue_raw_descriptor_read_request, Receiver>(
            context, kqueue_raw_descriptor_read_request(descriptor, buffer),
            std::move(receiver)) {}
};

/** Low-level streaming write operation backed by an objectized request. */
template <class Receiver>
class kqueue_write_operation
    : public detail::kqueue_ready_io_operation<
          kqueue_raw_descriptor_write_request, Receiver> {
 public:
  kqueue_write_operation(kqueue_context& context, descriptor_view descriptor,
                         buffer_view buffer, Receiver receiver)
      : detail::kqueue_ready_io_operation<
            kqueue_raw_descriptor_write_request, Receiver>(
            context, kqueue_raw_descriptor_write_request(descriptor, buffer),
            std::move(receiver)) {}
};

}  // namespace bnio::async_io::bsd_native

#endif  // BNIO_ASYNC_IO_BSD_KQUEUE_OPERATIONS_FILE_H_
