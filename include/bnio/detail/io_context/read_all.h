/**
 * @file read_all.h
 * @brief read-all composite async operation.
 */

#ifndef BNIO_DETAIL_IO_CONTEXT_READ_ALL_H_
#ifndef BNIO_DETAIL_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_IO_CONTEXT_READ_ALL_H_

// write_all.h defines the generic operation/sender/factory/predicate
// templates that are reused verbatim for read-all.  The only difference
// between read-all and write-all is the state class.
#include <bnio/detail/io_context/write_all.h>

namespace bnio::detail {

/**
 * State for a socket read-all operation.
 *
 * Its interface (remaining, current_buffer, advance, done, make_sender)
 * is identical to socket_write_all_state, so the same wire-up
 * templates – write_all_step_operation, write_all_step_sender,
 * write_all_step_factory, write_all_done_predicate, write_all_operation,
 * and write_all_sender – work unchanged.
 */
class socket_read_all_state {
 public:
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
        *context, make_stream_read_request(socket, current_buffer(), flags));
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
};

/**
 * State for a descriptor (file) read-all operation.
 */
class descriptor_read_all_state {
 public:
  descriptor_read_all_state(io_context& context,
                            async_io::descriptor_view descriptor,
                            mutable_buffer buffer,
                            std::uint64_t offset) noexcept
      : context(&context),
        descriptor(descriptor),
        buffer(buffer),
        offset(offset) {}

  [[nodiscard]] std::size_t remaining() const noexcept {
    return buffer.size() - transferred;
  }

  [[nodiscard]] bool empty() const noexcept { return buffer.size() == 0; }

  [[nodiscard]] mutable_buffer current_buffer() const noexcept {
    auto* data = static_cast<char*>(buffer.data());
    return mutable_buffer(data + transferred, remaining());
  }

  [[nodiscard]] auto make_sender() noexcept {
    return native_io_sender(*context,
                            make_file_read_request(descriptor, current_buffer(),
                                                   offset + transferred));
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
  std::uint64_t offset;
  std::size_t transferred = 0;
  bool done = false;
};

// read_all_sender reuses write_all_sender with a read-specific state.
template <class State>
using read_all_sender = write_all_sender<State>;

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_IO_CONTEXT_READ_ALL_H_
