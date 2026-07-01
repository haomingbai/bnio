#pragma once
#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_H_
#define BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_H_

#include <bupp/async_io/linux/socket_address.h>
#include <bupp/base/linux/submission_queue_entry.h>
#include <bupp/buffer.h>
#include <bupp/linux/io_context.h>
#include <bupp/tcp.h>
#include <liburing.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bupp {

/** @cond BUPP_DETAIL */
namespace detail {

[[nodiscard]] inline std::error_code errno_result(int result) noexcept {
  return std::error_code(-result, std::generic_category());
}

template <class Receiver>
[[nodiscard]] bool stop_requested(const Receiver& receiver) noexcept {
  auto env = bexec::get_env(receiver);
  auto token = bexec::query(env, bexec::get_stop_token);
  return token.stop_requested();
}

template <class Receiver>
class timer_wait_operation : public timer_operation_base {
 public:
  timer_wait_operation(steady_timer& timer, Receiver receiver)
      : timer_operation_base(timer.context()),
        timer_(&timer.timer_),
        receiver_(std::move(receiver)) {}

  void start() noexcept {
    if (stop_requested(receiver_)) {
      this->timer_completion_ = timer_completion_kind::stopped;
      (void)this->timer_context_->native_context().post(*this);
      return;
    }

    this->timer_context_->start_timer_wait(*this, *timer_);
  }

  void execute() noexcept override {
    const timer_completion_kind completion = this->timer_completion();
    if (completion == timer_completion_kind::stopped) {
      bexec::set_stopped(std::move(receiver_));
    } else {
      bexec::set_value(std::move(receiver_));
    }
  }

 private:
  detail::timer_slot* timer_;
  std::remove_cvref_t<Receiver> receiver_;
};

class timer_wait_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  explicit timer_wait_sender(steady_timer& timer) noexcept : timer_(&timer) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return timer_wait_operation<std::remove_cvref_t<Receiver>>(
        *timer_, std::move(receiver));
  }

  template <class Receiver>
  auto connect(Receiver receiver) const& {
    return timer_wait_operation<std::remove_cvref_t<Receiver>>(
        *timer_, std::move(receiver));
  }

 private:
  steady_timer* timer_;
};

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

class receive_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  receive_model(async_io::stream_socket_view socket, mutable_buffer buffer,
                int flags) noexcept
      : socket_(socket), buffer_(buffer), flags_(flags) {}

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
    bexec::set_value(std::forward<Receiver>(receiver),
                     static_cast<std::size_t>(result));
  }

 private:
  async_io::stream_socket_view socket_;
  mutable_buffer buffer_;
  int flags_;
};

class send_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  send_model(async_io::stream_socket_view socket, const_buffer buffer,
             int flags) noexcept
      : socket_(socket), buffer_(buffer), flags_(flags) {}

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
    sqe.prep_read(descriptor_.native_handle(), buffer_.data(),
                  static_cast<unsigned>(buffer_.size()), offset_);
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
    sqe.prep_write(descriptor_.native_handle(), buffer_.data(),
                   static_cast<unsigned>(buffer_.size()), offset_);
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

class poll_model {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(unsigned),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  poll_model(async_io::descriptor_view descriptor, unsigned poll_mask) noexcept
      : request_(descriptor, poll_mask) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept {
    request_.prepare(sqe);
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
                     static_cast<unsigned>(result));
  }

 private:
  async_io::linux_native::io_uring_poll_request request_;
};

}  // namespace detail
/** @endcond */

inline auto io_context::async_receive(async_io::stream_socket_view socket,
                                      mutable_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::receive_model(socket, buffer, flags), submit_mode::queued);
}

inline auto io_context::async_receive_direct(
    async_io::stream_socket_view socket, mutable_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::receive_model(socket, buffer, flags), submit_mode::direct);
}

inline auto io_context::async_send(async_io::stream_socket_view socket,
                                   const_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::send_model(socket, buffer, flags), submit_mode::queued);
}

inline auto io_context::async_send_direct(async_io::stream_socket_view socket,
                                          const_buffer buffer, int flags) {
  return detail::native_io_sender(
      *this, detail::send_model(socket, buffer, flags), submit_mode::direct);
}

inline auto io_context::async_read(async_io::descriptor_view descriptor,
                                   mutable_buffer buffer,
                                   std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::read_model(descriptor, buffer, offset),
      submit_mode::queued);
}

inline auto io_context::async_read_direct(async_io::descriptor_view descriptor,
                                          mutable_buffer buffer,
                                          std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::read_model(descriptor, buffer, offset),
      submit_mode::direct);
}

inline auto io_context::async_write(async_io::descriptor_view descriptor,
                                    const_buffer buffer, std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::write_model(descriptor, buffer, offset),
      submit_mode::queued);
}

inline auto io_context::async_write_direct(async_io::descriptor_view descriptor,
                                           const_buffer buffer,
                                           std::uint64_t offset) {
  return detail::native_io_sender(
      *this, detail::write_model(descriptor, buffer, offset),
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

inline auto io_context::async_poll(async_io::descriptor_view descriptor,
                                   unsigned poll_mask) {
  return detail::native_io_sender(
      *this, detail::poll_model(descriptor, poll_mask), submit_mode::queued);
}

inline auto io_context::async_poll_direct(async_io::descriptor_view descriptor,
                                          unsigned poll_mask) {
  return detail::native_io_sender(
      *this, detail::poll_model(descriptor, poll_mask), submit_mode::direct);
}

inline auto io_context::async_resolve(async_io::dns_query query,
                                      async_io::dns_result_view result) {
  return native_context_.async_resolve(std::move(query), result);
}

inline auto io_context::async_resolve(std::string_view host,
                                      std::string_view service,
                                      async_io::dns_result_view result) {
  return async_resolve(async_io::dns_query(host, service), result);
}

inline auto steady_timer::async_wait() {
  return detail::timer_wait_sender(*this);
}

}  // namespace bupp

#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_H_
