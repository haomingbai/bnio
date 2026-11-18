/**
 * @file read_all.h
 * @brief read-all composite async operation.
 */

#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_READ_ALL_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_POSIX_IO_CONTEXT_READ_ALL_H_

// write_all.h holds the generic operation/sender/factory/predicate templates
// reused for read-all; only the state class differs.
#include <bnio/detail/posix/io_context/write_all.h>

namespace bnio::detail {

/**
 * State for a socket read-all operation. It exposes the same interface as
 * socket_write_all_state, so the write_all wire-up templates are reused
 * unchanged.
 */
template <io_context::schedule_kind Kind>
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
    return make_io_sender<Kind>(
        *context, make_stream_read_request(socket, current_buffer(), flags),
        adaptive_eager_control<socket_read_all_state<Kind> >{this});
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
  // Cleared after a short transfer, so the next step skips the eager probe.
  bool eager = true;
};

// Local (AF_UNIX) twin of socket_read_all_state: identical wire-up, but
// the step operation goes through the local read factory.
template <io_context::schedule_kind Kind>
class local_socket_read_all_state {
 public:
  static constexpr bool zero_byte_is_error = false;

  local_socket_read_all_state(io_context& context,
                              async_io::local::stream_socket_view socket,
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
    return make_io_sender<Kind>(
        *context,
        make_local_stream_read_request(socket, current_buffer(), flags),
        adaptive_eager_control<local_socket_read_all_state<Kind> >{this});
  }

  void advance(std::size_t bytes) noexcept {
    transferred += bytes;
    if (transferred >= buffer.size()) {
      done = true;
    }
  }

  io_context* context;
  async_io::local::stream_socket_view socket;
  mutable_buffer buffer;
  int flags;
  std::size_t transferred = 0;
  bool done = false;
  // Cleared after a short transfer, so the next step skips the eager probe.
  bool eager = true;
};

}  // namespace bnio::detail

#include <bnio/detail/posix/io_context/random_access_read_all.h>
#include <bnio/detail/posix/io_context/stream_file_read_all.h>

namespace bnio::detail {

// read_all_sender reuses write_all_sender with a read-specific state.
template <class State>
using read_all_sender = write_all_sender<State>;

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_READ_ALL_H_
