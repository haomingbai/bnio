/**
 * @file random_access_read_all.h
 * @brief Random access file read-all operation state.
 */

#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_RANDOM_ACCESS_READ_ALL_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_POSIX_IO_CONTEXT_RANDOM_ACCESS_READ_ALL_H_

namespace bnio::detail {

/**
 * State for a positioned read-all operation on a random access file.
 *
 * Every step passes an explicit offset (offset + transferred); the kernel
 * file position is never observed or advanced.
 */
class random_access_read_all_state {
 public:
  static constexpr bool zero_byte_is_error = false;

  random_access_read_all_state(io_context& context,
                               async_io::random_access_file file,
                               mutable_buffer buffer,
                               std::uint64_t offset) noexcept
      : context(&context), file(file), buffer(buffer), offset(offset) {}

  [[nodiscard]] std::size_t remaining() const noexcept {
    return buffer.size() - transferred;
  }

  [[nodiscard]] bool empty() const noexcept { return buffer.size() == 0; }

  [[nodiscard]] mutable_buffer current_buffer() const noexcept {
    auto* data = static_cast<char*>(buffer.data());
    return mutable_buffer(data + transferred, remaining());
  }

  [[nodiscard]] auto make_sender() noexcept {
    return native_io_sender(
        *context,
        make_random_access_read_request(file, current_buffer(),
                                        offset + transferred),
        adaptive_eager_control<random_access_read_all_state>{this});
  }

  void advance(std::size_t bytes) noexcept {
    transferred += bytes;
    if (transferred >= buffer.size()) {
      done = true;
    }
  }

  io_context* context;
  async_io::random_access_file file;
  mutable_buffer buffer;
  std::uint64_t offset;
  std::size_t transferred = 0;
  bool done = false;
  // Adaptive eager probing: cleared when the previous step had a short
  // transfer, so the next step skips the immediate-completion probe.
  bool eager = true;
};

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_RANDOM_ACCESS_READ_ALL_H_
