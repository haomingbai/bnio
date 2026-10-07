/**
 * @file socket.h
 * @brief Linux native socket I/O operations.
 */

#ifndef BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_SOCKET_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_SOCKET_H_

#include <bnio/async_io/linux/socket_address.h>
#include <bnio/async_io/local/endpoint.h>
#include <bnio/async_io/local/socket_view.h>

#include <algorithm>
#include <system_error>

namespace bnio::detail {

// Socket models are descriptor-typed and family-agnostic: the factories
// overload on the view type (network vs local) and hand the models a plain
// descriptor, so one model body serves both families.

class socket_read_model {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  socket_read_model(int descriptor, mutable_buffer buffer, int flags) noexcept
      : descriptor_(descriptor), buffer_(buffer), flags_(flags) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    const async_io::buffer_view view = buffer_.view();
    sqe.prep_recv(descriptor_, view.data,
                  async_io::linux_native::detail::bounded_io_size(view.size),
                  flags_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const async_io::buffer_view view = buffer_.view();
    const ssize_t result =
        ::recv(descriptor_, view.data,
               async_io::linux_native::detail::bounded_io_size(view.size),
               flags_ | MSG_DONTWAIT);
    return immediate_socket_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  int descriptor_;
  mutable_buffer buffer_;
  int flags_;
};

class socket_write_model {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  socket_write_model(int descriptor, const_buffer buffer, int flags) noexcept
      : descriptor_(descriptor), buffer_(buffer), flags_(flags) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_send(
        descriptor_, buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result =
        ::send(descriptor_, buffer_.data(),
               async_io::linux_native::detail::bounded_io_size(buffer_.size()),
               flags_ | MSG_DONTWAIT);
    return immediate_socket_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  int descriptor_;
  const_buffer buffer_;
  int flags_;
};

class datagram_receive_model {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  datagram_receive_model(int descriptor, mutable_buffer buffer,
                         int flags) noexcept
      : descriptor_(descriptor), buffer_(buffer), flags_(flags) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_recv(
        descriptor_, buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result =
        ::recv(descriptor_, buffer_.data(),
               async_io::linux_native::detail::bounded_io_size(buffer_.size()),
               flags_ | MSG_DONTWAIT);
    return immediate_socket_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  int descriptor_;
  mutable_buffer buffer_;
  int flags_;
};

class datagram_send_model {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  datagram_send_model(int descriptor, const_buffer buffer, int flags) noexcept
      : descriptor_(descriptor), buffer_(buffer), flags_(flags) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_send(
        descriptor_, buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_);
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result =
        ::send(descriptor_, buffer_.data(),
               async_io::linux_native::detail::bounded_io_size(buffer_.size()),
               flags_ | MSG_DONTWAIT);
    return immediate_socket_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  int descriptor_;
  const_buffer buffer_;
  int flags_;
};

class datagram_receive_from_model {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  datagram_receive_from_model(int descriptor, mutable_buffer buffer,
                              ip::endpoint& endpoint, int flags) noexcept
      : descriptor_(descriptor),
        buffer_(buffer),
        endpoint_(&endpoint),
        flags_(flags) {}

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
    sqe.prep_recvmsg(descriptor_, &message_, static_cast<unsigned>(flags_));
  }

  [[nodiscard]] int try_immediate() noexcept {
    remote_address_ = {};
    socklen_t size = sizeof(remote_address_);
    const ssize_t result = ::recvfrom(
        descriptor_, buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_ | MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&remote_address_),
        &size);
    if (result >= 0) {
      message_.msg_namelen = size;
    }
    return immediate_socket_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    if (result >= 0 && !ec) {
      const auto endpoint = async_io::linux_native::make_endpoint(
          reinterpret_cast<const sockaddr*>(&remote_address_),
          message_.msg_namelen);
      if (!endpoint.has_value()) {
        // endpoint decode failure: override ec with
        // address_family_not_supported
        endpoint_->reset();
        bexec::set_value(
            std::forward<Receiver>(receiver),
            std::make_error_code(std::errc::address_family_not_supported),
            std::size_t{0});
        return;
      }
      *endpoint_ = *endpoint;
    }
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  int descriptor_;
  mutable_buffer buffer_;
  ip::endpoint* endpoint_;
  sockaddr_storage remote_address_{};
  iovec buffer_entry_{};
  msghdr message_{};
  int flags_;
};

class local_datagram_receive_from_model {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  local_datagram_receive_from_model(int descriptor, mutable_buffer buffer,
                                    async_io::local::endpoint& endpoint,
                                    int flags) noexcept
      : descriptor_(descriptor),
        buffer_(buffer),
        endpoint_(&endpoint),
        flags_(flags) {}

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
    sqe.prep_recvmsg(descriptor_, &message_, static_cast<unsigned>(flags_));
  }

  [[nodiscard]] int try_immediate() noexcept {
    remote_address_ = {};
    socklen_t size = sizeof(remote_address_);
    const ssize_t result = ::recvfrom(
        descriptor_, buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_ | MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&remote_address_),
        &size);
    if (result >= 0) {
      message_.msg_namelen = size;
    }
    return immediate_socket_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    if (result >= 0 && !ec) {
      const auto endpoint = async_io::linux_native::make_local_endpoint(
          reinterpret_cast<const sockaddr*>(&remote_address_),
          message_.msg_namelen);
      if (!endpoint.has_value()) {
        // endpoint decode failure: override ec with
        // address_family_not_supported
        *endpoint_ = async_io::local::endpoint();
        bexec::set_value(
            std::forward<Receiver>(receiver),
            std::make_error_code(std::errc::address_family_not_supported),
            std::size_t{0});
        return;
      }
      *endpoint_ = *endpoint;
    }
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  int descriptor_;
  mutable_buffer buffer_;
  async_io::local::endpoint* endpoint_;
  sockaddr_storage remote_address_{};
  iovec buffer_entry_{};
  msghdr message_{};
  int flags_;
};

class datagram_send_to_model {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  datagram_send_to_model(int descriptor, const_buffer buffer,
                         const ip::endpoint& endpoint, int flags)
      : descriptor_(descriptor),
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
    sqe.prep_sendmsg(descriptor_, &message_, static_cast<unsigned>(flags_));
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result = ::sendto(
        descriptor_, buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_ | MSG_DONTWAIT, remote_address_.data(), remote_address_.size());
    return immediate_socket_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  int descriptor_;
  const_buffer buffer_;
  async_io::linux_native::socket_address remote_address_;
  iovec buffer_entry_{};
  msghdr message_{};
  int flags_;
};

class local_datagram_send_to_model {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  local_datagram_send_to_model(int descriptor, const_buffer buffer,
                               const async_io::local::endpoint& endpoint,
                               int flags)
      : descriptor_(descriptor),
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
    sqe.prep_sendmsg(descriptor_, &message_, static_cast<unsigned>(flags_));
  }

  [[nodiscard]] int try_immediate() noexcept {
    const ssize_t result = ::sendto(
        descriptor_, buffer_.data(),
        async_io::linux_native::detail::bounded_io_size(buffer_.size()),
        flags_ | MSG_DONTWAIT, remote_address_.data(), remote_address_.size());
    return immediate_socket_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec,
                     static_cast<std::size_t>(std::max(0, result)));
  }

 private:
  int descriptor_;
  const_buffer buffer_;
  async_io::linux_native::socket_address remote_address_;
  iovec buffer_entry_{};
  msghdr message_{};
  int flags_;
};

class accept_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code, int),
                                   bexec::set_stopped_t()>;

  accept_model(int descriptor, int flags) noexcept
      : descriptor_(descriptor), flags_(flags) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_accept(descriptor_, nullptr, nullptr, flags_);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int result,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec, result);
  }

 private:
  int descriptor_;
  int flags_;
};

class connect_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code),
                                   bexec::set_stopped_t()>;

  connect_model(int descriptor, const ip::endpoint& endpoint)
      : descriptor_(descriptor), address_(endpoint) {}

  connect_model(int descriptor, const async_io::local::endpoint& endpoint)
      : descriptor_(descriptor), address_(endpoint) {}

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_connect(descriptor_, address_.data(), address_.size());
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, std::error_code ec, int,
                 unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver), ec);
  }

 private:
  int descriptor_;
  async_io::linux_native::socket_address address_;
};

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_SOCKET_H_
