#pragma once
#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_H_
#define BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_H_

#include <bupp/async_io/linux/socket_address.h>
#include <bupp/base/submission_queue_entry.h>
#include <bupp/buffer.h>
#include <bupp/linux/io_context.h>
#include <bupp/tcp.h>
#include <liburing.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bupp {

/** @cond BUPP_DETAIL */
namespace detail {

[[nodiscard]] constexpr __kernel_timespec to_kernel_timespec(
    async_io::duration value) noexcept {
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(value);
  auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(value - seconds);
  if (nanoseconds.count() < 0) {
    --seconds;
    nanoseconds += std::chrono::seconds(1);
  }

  __kernel_timespec result{};
  result.tv_sec = static_cast<decltype(result.tv_sec)>(seconds.count());
  result.tv_nsec = static_cast<decltype(result.tv_nsec)>(nanoseconds.count());
  return result;
}

[[nodiscard]] inline std::error_code errno_result(int result) noexcept {
  return std::error_code(-result, std::generic_category());
}

template <class Receiver>
[[nodiscard]] bool stop_requested(const Receiver& receiver) noexcept {
  auto env = bexec::get_env(receiver);
  auto token = bexec::query(env, bexec::get_stop_token);
  return token.stop_requested();
}

template <class Model, class Receiver>
class native_io_operation : public io_context::operation_base {
 public:
  native_io_operation(io_context& context, Model model, submit_mode mode,
                      Receiver receiver)
      : context_(&context),
        model_(std::move(model)),
        mode_(mode),
        receiver_(std::move(receiver)) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    model_.prepare(sqe);
  }

  [[nodiscard]] int prepare_for_submit() noexcept override {
    completion_ = completion_kind::value;
    return context_->native_context().prepare(*this);
  }

  void complete_submit_error(int result) noexcept override {
    completion_ = completion_kind::error;
    error_ = errno_result(result);
  }

  void start() noexcept {
    if (stop_requested(receiver_)) {
      completion_ = completion_kind::stopped;
      context_->post(*this);
      return;
    }

    completion_ = completion_kind::value;
    if (mode_ == submit_mode::direct) {
      context_->submit_direct(*this);
    } else {
      context_->enqueue_io(*this);
    }
  }

  void execute() noexcept override {
    switch (completion_) {
      case completion_kind::value:
        if (model_.is_error_result(this->result)) {
          bexec::set_error(std::move(receiver_),
                           model_.make_error(this->result));
        } else {
          model_.set_value(std::move(receiver_), this->result, this->flags);
        }
        break;
      case completion_kind::error:
        bexec::set_error(std::move(receiver_), error_);
        break;
      case completion_kind::stopped:
        bexec::set_stopped(std::move(receiver_));
        break;
    }
  }

 private:
  enum class completion_kind {
    value,
    error,
    stopped,
  };

  io_context* context_;
  Model model_;
  submit_mode mode_;
  std::remove_cvref_t<Receiver> receiver_;
  completion_kind completion_ = completion_kind::value;
  std::error_code error_;
};

template <class Model>
class native_io_sender {
 public:
  using completion_signatures = typename Model::completion_signatures;

  native_io_sender(io_context& context, Model model, submit_mode mode) noexcept
      : context_(&context), model_(std::move(model)), mode_(mode) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return native_io_operation<Model, std::remove_cvref_t<Receiver>>(
        *context_, std::move(model_), mode_, std::move(receiver));
  }

  template <class Receiver>
    requires std::copy_constructible<Model>
  auto connect(Receiver receiver) const& {
    return native_io_operation<Model, std::remove_cvref_t<Receiver>>(
        *context_, model_, mode_, std::move(receiver));
  }

 private:
  io_context* context_;
  Model model_;
  submit_mode mode_;
};

template <class Holder>
class receive_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  receive_model(async_io::stream_socket_view socket, Holder buffer,
                int flags) noexcept
      : socket_(socket), buffer_(std::move(buffer)), flags_(flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    const async_io::buffer_view view = buffer_.view();
    sqe.prep_recv(socket_.native_handle(), view.data, view.size, flags_);
  }

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0;
  }

  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int result, unsigned) noexcept {
    const auto size = static_cast<std::size_t>(result);
    buffer_.commit(size);
    bexec::set_value(std::forward<Receiver>(receiver), size);
  }

 private:
  async_io::stream_socket_view socket_;
  Holder buffer_;
  int flags_;
};

template <class Holder>
class send_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  send_model(async_io::stream_socket_view socket, Holder buffer,
             int flags) noexcept
      : socket_(socket), buffer_(std::move(buffer)), flags_(flags) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_send(socket_.native_handle(), buffer_.data(), buffer_.size(),
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
  Holder buffer_;
  int flags_;
};

class accept_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(tcp_socket),
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
    bexec::set_value(std::forward<Receiver>(receiver), tcp_socket(result));
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

class wait_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  explicit wait_model(async_io::duration timeout) noexcept
      : timeout_(to_kernel_timespec(timeout)) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    sqe.prep_timeout(&timeout_, 0, 0);
  }

  [[nodiscard]] bool is_error_result(int result) const noexcept {
    return result < 0 && result != -ETIME;
  }

  [[nodiscard]] std::error_code make_error(int result) const noexcept {
    return errno_result(result);
  }

  template <class Receiver>
  void set_value(Receiver&& receiver, int, unsigned) noexcept {
    bexec::set_value(std::forward<Receiver>(receiver));
  }

 private:
  __kernel_timespec timeout_;
};

}  // namespace detail
/** @endcond */

template <class Buffer>
auto io_context::async_receive(async_io::stream_socket_view socket,
                               Buffer&& buffer, int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using holder_type = decltype(holder);
  return detail::native_io_sender(
      *this,
      detail::receive_model<holder_type>(socket, std::move(holder), flags),
      submit_mode::queued);
}

template <class Buffer>
auto io_context::async_receive(tcp_socket& socket, Buffer&& buffer, int flags) {
  return async_receive(socket.view(), std::forward<Buffer>(buffer), flags);
}

template <class Buffer>
auto io_context::async_receive_direct(async_io::stream_socket_view socket,
                                      Buffer&& buffer, int flags) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using holder_type = decltype(holder);
  return detail::native_io_sender(
      *this,
      detail::receive_model<holder_type>(socket, std::move(holder), flags),
      submit_mode::direct);
}

template <class Buffer>
auto io_context::async_receive_direct(tcp_socket& socket, Buffer&& buffer,
                                      int flags) {
  return async_receive_direct(socket.view(), std::forward<Buffer>(buffer),
                              flags);
}

template <class Buffer>
auto io_context::async_send(async_io::stream_socket_view socket,
                            Buffer&& buffer, int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using holder_type = decltype(holder);
  return detail::native_io_sender(
      *this, detail::send_model<holder_type>(socket, std::move(holder), flags),
      submit_mode::queued);
}

template <class Buffer>
auto io_context::async_send(tcp_socket& socket, Buffer&& buffer, int flags) {
  return async_send(socket.view(), std::forward<Buffer>(buffer), flags);
}

template <class Buffer>
auto io_context::async_send_direct(async_io::stream_socket_view socket,
                                   Buffer&& buffer, int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using holder_type = decltype(holder);
  return detail::native_io_sender(
      *this, detail::send_model<holder_type>(socket, std::move(holder), flags),
      submit_mode::direct);
}

template <class Buffer>
auto io_context::async_send_direct(tcp_socket& socket, Buffer&& buffer,
                                   int flags) {
  return async_send_direct(socket.view(), std::forward<Buffer>(buffer), flags);
}

template <class Rep, class Period>
auto io_context::async_wait(std::chrono::duration<Rep, Period> timeout) {
  return detail::native_io_sender(
      *this,
      detail::wait_model(
          std::chrono::duration_cast<async_io::duration>(timeout)),
      submit_mode::queued);
}

template <class Rep, class Period>
auto io_context::async_wait_direct(std::chrono::duration<Rep, Period> timeout) {
  return detail::native_io_sender(
      *this,
      detail::wait_model(
          std::chrono::duration_cast<async_io::duration>(timeout)),
      submit_mode::direct);
}

inline auto io_context::async_accept(async_io::listening_socket_view socket,
                                     int flags) {
  return detail::native_io_sender(*this, detail::accept_model(socket, flags),
                                  submit_mode::queued);
}

inline auto io_context::async_accept_direct(
    async_io::listening_socket_view socket, int flags) {
  return detail::native_io_sender(*this, detail::accept_model(socket, flags),
                                  submit_mode::direct);
}

inline auto io_context::async_accept(tcp_acceptor& acceptor, int flags) {
  return async_accept(acceptor.view(), flags);
}

inline auto io_context::async_accept_direct(tcp_acceptor& acceptor, int flags) {
  return async_accept_direct(acceptor.view(), flags);
}

inline auto io_context::async_connect(async_io::stream_socket_view socket,
                                      const ip::endpoint& endpoint) {
  return detail::native_io_sender(
      *this, detail::connect_model(socket, endpoint), submit_mode::queued);
}

inline auto io_context::async_connect_direct(
    async_io::stream_socket_view socket, const ip::endpoint& endpoint) {
  return detail::native_io_sender(
      *this, detail::connect_model(socket, endpoint), submit_mode::direct);
}

inline auto io_context::async_connect(tcp_socket& socket,
                                      const ip::endpoint& endpoint) {
  return async_connect(socket.view(), endpoint);
}

inline auto io_context::async_connect_direct(tcp_socket& socket,
                                             const ip::endpoint& endpoint) {
  return async_connect_direct(socket.view(), endpoint);
}

}  // namespace bupp

#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_H_
