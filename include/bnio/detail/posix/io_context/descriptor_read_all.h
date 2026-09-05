/**
 * @file descriptor_read_all.h
 * @brief Streaming descriptor read-all operation state.
 */

#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_DESCRIPTOR_READ_ALL_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_POSIX_IO_CONTEXT_DESCRIPTOR_READ_ALL_H_

namespace bnio::detail {

/**
 * State for a streaming descriptor read-all operation.
 *
 * Each step reads at the kernel file position, which the kernel advances
 * naturally on every successful read.
 */
class descriptor_read_all_state {
 public:
  static constexpr bool zero_byte_is_error = false;

  descriptor_read_all_state(io_context& context,
                            async_io::descriptor_view descriptor,
                            mutable_buffer buffer) noexcept
      : context(&context), descriptor(descriptor), buffer(buffer) {}

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
        *context, make_descriptor_read_request(descriptor, current_buffer()),
        adaptive_eager_control<descriptor_read_all_state>{this});
  }

  void advance(std::size_t bytes) noexcept {
    transferred += bytes;
    if (transferred >= buffer.size()) {
      done = true;
    }
  }

  io_context* context;
  async_io::descriptor_view descriptor;
  mutable_buffer buffer;
  std::size_t transferred = 0;
  bool done = false;
  // Adaptive eager probing: cleared when the previous step had a short
  // transfer, so the next step skips the immediate-completion probe.
  bool eager = true;
};

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_DESCRIPTOR_READ_ALL_H_
