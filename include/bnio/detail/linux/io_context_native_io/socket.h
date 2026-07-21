#ifndef BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_SOCKET_H_
#ifndef BNIO_DETAIL_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_SOCKET_H_

namespace bnio::detail {

class socket_read_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  socket_read_model(async_io::stream_socket_view socket, mutable_buffer buffer,
                    int flags) noexcept
      : socket_(socket), buffer_(buffer), flags_(flags) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    const async_io::buffer_view view = buffer_.view();
    sqe.prep_recv(socket_.native_handle(), view.data,
                  async_io::linux_native::detail::bounded_io_size(view.size),
                  flags_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const async_io::buffer_view view = buffer_.view();
    const ssize_t result =
        ::recv(socket_.native_handle(), view.data,
               async_io::linux_native::detail::bounded_io_size(view.size),
               flags_ | MSG_DONTWAIT);
    return immediate_socket_result(result);
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

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_send(
        socket_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result =
        ::send(socket_.native_handle(), buffer_.data(),
               async_io::linux_native::detail::bounded_io_size(buffer_.size()),
               flags_ | MSG_DONTWAIT);
    return immediate_socket_result(result);
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

class datagram_receive_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  datagram_receive_model(async_io::datagram_socket_view socket,
                         mutable_buffer buffer, int flags) noexcept
      : socket_(socket), buffer_(buffer), flags_(flags) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_recv(
        socket_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result =
        ::recv(socket_.native_handle(), buffer_.data(),
               async_io::linux_native::detail::bounded_io_size(buffer_.size()),
               flags_ | MSG_DONTWAIT);
    return immediate_socket_result(result);
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
  async_io::datagram_socket_view socket_;
  mutable_buffer buffer_;
  int flags_;
};

class datagram_send_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  datagram_send_model(async_io::datagram_socket_view socket,
                      const_buffer buffer, int flags) noexcept
      : socket_(socket), buffer_(buffer), flags_(flags) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_send(
        socket_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result =
        ::send(socket_.native_handle(), buffer_.data(),
               async_io::linux_native::detail::bounded_io_size(buffer_.size()),
               flags_ | MSG_DONTWAIT);
    return immediate_socket_result(result);
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
  async_io::datagram_socket_view socket_;
  const_buffer buffer_;
  int flags_;
};

class datagram_receive_from_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  datagram_receive_from_model(async_io::datagram_socket_view socket,
                              mutable_buffer buffer, ip::endpoint& endpoint,
                              int flags) noexcept
      : socket_(socket), buffer_(buffer), endpoint_(&endpoint), flags_(flags) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    remote_address_ = {};
    buffer_entry_ = {
        buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size())};
    message_ = {};
    message_.msg_name = &remote_address_;
    message_.msg_namelen = sizeof(remote_address_);
    message_.msg_iov = &buffer_entry_;
    message_.msg_iovlen = 1;
    sqe.prep_recvmsg(socket_.native_handle(), &message_,
                     static_cast<unsigned>(flags_));
  }

  [[nodiscard]] int try_immediate() noexcept {
    remote_address_ = {};
    socklen_t size = sizeof(remote_address_);
    const ssize_t result = ::recvfrom(
        socket_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_ | MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&remote_address_),
        &size);
    if (result >= 0) {
      message_.msg_namelen = size;
    }
    return immediate_socket_result(result);
  }

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }

  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    const auto endpoint = async_io::linux_native::make_endpoint(
        reinterpret_cast<const sockaddr*>(&remote_address_),
        message_.msg_namelen);
    if (!endpoint.has_value()) {
      endpoint_->reset();
      bexec::set_error(
          std::forward<Receiver>(receiver),
          std::make_error_code(std::errc::address_family_not_supported));
      return;
    }
    *endpoint_ = *endpoint;
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<std::size_t>(result));
  }

 private:
  async_io::datagram_socket_view socket_;
  mutable_buffer buffer_;
  ip::endpoint* endpoint_;
  sockaddr_storage remote_address_{};
  iovec buffer_entry_{};
  msghdr message_{};
  int flags_;
};

class datagram_send_to_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  datagram_send_to_model(async_io::datagram_socket_view socket,
                         const_buffer buffer, const ip::endpoint& endpoint,
                         int flags)
      : socket_(socket),
        buffer_(buffer),
        remote_address_(endpoint),
        flags_(flags) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    buffer_entry_ = {
        const_cast<void*>(buffer_.data()),
        async_io::linux_native::detail::bounded_io_size(buffer_.size())};
    message_ = {};
    message_.msg_name = const_cast<sockaddr*>(remote_address_.data());
    message_.msg_namelen = remote_address_.size();
    message_.msg_iov = &buffer_entry_;
    message_.msg_iovlen = 1;
    sqe.prep_sendmsg(socket_.native_handle(), &message_,
                     static_cast<unsigned>(flags_));
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result = ::sendto(
        socket_.native_handle(), buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_ | MSG_DONTWAIT, remote_address_.data(), remote_address_.size());
    return immediate_socket_result(result);
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
  async_io::datagram_socket_view socket_;
  const_buffer buffer_;
  async_io::linux_native::socket_address remote_address_;
  iovec buffer_entry_{};
  msghdr message_{};
  int flags_;
};

class accept_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(int),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  accept_model(async_io::stream_socket_view socket, int flags) noexcept
      : socket_(socket), flags_(flags) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
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
  async_io::stream_socket_view socket_;
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

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
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

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_SOCKET_H_
