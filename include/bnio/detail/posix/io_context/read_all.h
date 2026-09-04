/**
 * @file read_all.h
 * @brief read-all composite async operation.
 */

#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_READ_ALL_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_POSIX_IO_CONTEXT_READ_ALL_H_

// write_all.h defines the generic operation/sender/factory/predicate
// templates that are reused verbatim for read-all.  The only difference
// between read-all and write-all is the state class.
#include <bnio/detail/posix/io_context/write_all.h>

namespace bnio::detail {

/**
 * State for a socket read-all operation.
 *
 * Its interface (remaining, current_buffer, advance, done, make_sender)
 * is identical to socket_write_all_state, so the same wire-up
 * templates – write_all_step_complete, write_all_step_factory,
 * write_all_done_predicate, write_all_operation,
 * and write_all_sender – work unchanged.
 */
class socket_read_all_state {
 public:
  static constexpr bool zero_byte_is_error = false;

  socket_read_all_state(io_context& context,
                        async_io::stream_socket_view socket,
                        mutable_buffer buffer, int flags) noexcept
      : context(&context), socket(socket), buffer(buffer), flags(flags) {}

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
        *context, make_stream_read_request(socket, current_buffer(), flags),
        adaptive_eager_control<socket_read_all_state>{this});
  }

  void advance(std::size_t bytes) noexcept {
    transferred += bytes;
    if (transferred >= buffer.size()) {
      done = true;
    }
  }

  io_context* context;
  async_io::stream_socket_view socket;
  mutable_buffer buffer;
  int flags;
  std::size_t transferred = 0;
  bool done = false;
  // Adaptive eager probing: cleared when the previous step had a short
  // transfer, so the next step skips the immediate-completion probe.
  bool eager = true;
};

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

// read_all_sender reuses write_all_sender with a read-specific state.
template <class State>
using read_all_sender = write_all_sender<State>;

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_READ_ALL_H_
