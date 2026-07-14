#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_FILE_H_
#ifndef BUPP_LINUX_IO_CONTEXT_H_
#include <bupp/linux/io_context.h>
#else
#define BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_FILE_H_

namespace bupp::detail {

class read_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  read_model(async_io::descriptor_view descriptor, mutable_buffer buffer,
             std::uint64_t offset) noexcept
      : descriptor_(descriptor), buffer_(buffer), offset_(offset) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_read(
        descriptor_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        offset_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result = pread_nowait(
        descriptor_.native_handle(), buffer_.data(),
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

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }

  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<std::size_t>(result));
  }

 private:
  async_io::descriptor_view descriptor_;
  mutable_buffer buffer_;
  std::uint64_t offset_;
};

class write_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  write_model(async_io::descriptor_view descriptor, const_buffer buffer,
              std::uint64_t offset) noexcept
      : descriptor_(descriptor), buffer_(buffer), offset_(offset) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_write(
        descriptor_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        offset_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result = pwrite_nowait(
        descriptor_.native_handle(), buffer_.data(),
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

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }

  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<std::size_t>(result));
  }

 private:
  async_io::descriptor_view descriptor_;
  const_buffer buffer_;
  std::uint64_t offset_;
};

}  // namespace bupp::detail

#endif  // BUPP_LINUX_IO_CONTEXT_H_
#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_FILE_H_
