#pragma once
#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_SOCKET_H_
#define BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_SOCKET_H_

#include <bupp/async_io/linux/socket_address.h>
#include <bupp/async_io/socket_view.h>
#include <bupp/base/linux/submission_queue_entry.h>
#include <bupp/buffer.h>
#include <bupp/ip.h>
#include <bupp/linux/detail/io_context_native_io/common.h>

#include <bexec/completion_signatures.hpp>
#include <cstddef>
#include <system_error>
#include <utility>

namespace bupp {

/** @cond BUPP_DETAIL */
namespace detail {

class socket_read_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  socket_read_model(async_io::stream_socket_view socket, mutable_buffer buffer,
                    int flags) noexcept
      : socket_(socket), buffer_(buffer), flags_(flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    const async_io::buffer_view view = buffer_.view();
    sqe.prep_recv(socket_.native_handle(), view.data,
                  async_io::linux_native::detail::bounded_io_size(view.size),
                  flags_);
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
  async_io::stream_socket_view socket_;
  mutable_buffer buffer_;
  int flags_;
};

class socket_write_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  socket_write_model(async_io::stream_socket_view socket, const_buffer buffer,
                     int flags) noexcept
      : socket_(socket), buffer_(buffer), flags_(flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_send(
        socket_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_);
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
  async_io::stream_socket_view socket_;
  const_buffer buffer_;
  int flags_;
};

class accept_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(int),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  accept_model(async_io::listening_socket_view socket, int flags) noexcept
      : socket_(socket), flags_(flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_accept(socket_.native_handle(), nullptr, nullptr, flags_);
  }

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }

  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), result);
  }

 private:
  async_io::listening_socket_view socket_;
  int flags_;
};

class connect_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  connect_model(async_io::stream_socket_view socket,
                const ip::endpoint& endpoint)
      : socket_(socket), address_(endpoint) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_connect(socket_.native_handle(), address_.data(), address_.size());
  }

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }

  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver));
  }

 private:
  async_io::stream_socket_view socket_;
  async_io::linux_native::socket_address address_;
};

}  // namespace detail
/** @endcond */

}  // namespace bupp

#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_SOCKET_H_
