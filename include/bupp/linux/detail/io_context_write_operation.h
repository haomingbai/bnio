#pragma once
#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_WRITE_OPERATION_H_
#define BUPP_LINUX_DETAIL_IO_CONTEXT_WRITE_OPERATION_H_

#include <bupp/buffer.h>
#include <bupp/linux/io_context.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/receiver.hpp>
#include <cerrno>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bupp {
/** @cond BUPP_DETAIL */
namespace detail {

/**
 * Composed operation that loops async_send until all bytes are written.
 *
 * This is the equivalent of asio::async_write — the caller provides a buffer
 * and receives a single completion when every byte has been handed to the
 * kernel (or an error / stop occurs).
 */
template <class Receiver>
class async_write_operation : public io_context::operation_base {
 public:
  async_write_operation(io_context& ctx, async_io::stream_socket_view socket,
                        const_buffer_holder holder, int flags,
                        Receiver receiver) noexcept
      : ctx_(&ctx),
        socket_(socket),
        holder_(std::move(holder)),
        flags_(flags),
        receiver_(std::move(receiver)),
        total_(holder_.size()) {}

  async_write_operation(const async_write_operation&) = delete;
  async_write_operation& operator=(const async_write_operation&) = delete;
  async_write_operation(async_write_operation&&) = delete;
  async_write_operation& operator=(async_write_operation&&) = delete;

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_send(socket_.native_handle(),
                  static_cast<const char*>(holder_.data()) + offset_,
                  total_ - offset_, flags_);
  }

  [[nodiscard]] int prepare_for_submit() noexcept override {
    return ctx_->native_context().prepare(*this);
  }

  void complete_submit_error(int result) noexcept override {
    bexec::set_error(std::move(receiver_), errno_result(result));
  }

  void start() noexcept {
    ctx_->enqueue_io(*this);
  }

  void execute() noexcept override {
    if (this->result < 0) {
      bexec::set_error(std::move(receiver_), errno_result(this->result));
      return;
    }

    offset_ += static_cast<std::size_t>(this->result);

    if (offset_ >= total_) {
      bexec::set_value(std::move(receiver_), offset_);
      return;
    }

    // More data to send — re-submit with updated offset.
    // Using submit_direct avoids re-entering the pending-I/O queue.
    ctx_->native_context().submit(*this);
  }

 private:
  io_context* ctx_;
  async_io::stream_socket_view socket_;
  const_buffer_holder holder_;
  int flags_;
  std::remove_cvref_t<Receiver> receiver_;
  std::size_t offset_ = 0;
  std::size_t total_;
};

/**
 * Sender returned by io_context::async_write().
 */
class async_write_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  async_write_sender(io_context& ctx, async_io::stream_socket_view socket,
                     const_buffer_holder holder, int flags) noexcept
      : ctx_(&ctx),
        socket_(socket),
        holder_(std::move(holder)),
        flags_(flags) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return async_write_operation<std::remove_cvref_t<Receiver>>(
        *ctx_, socket_, std::move(holder_), flags_, std::move(receiver));
  }

  template <class Receiver>
    requires std::copy_constructible<const_buffer_holder>
  auto connect(Receiver receiver) const& {
    return async_write_operation<std::remove_cvref_t<Receiver>>(
        *ctx_, socket_, holder_, flags_, std::move(receiver));
  }

 private:
  io_context* ctx_;
  async_io::stream_socket_view socket_;
  const_buffer_holder holder_;
  int flags_;
};

}  // namespace detail
/** @endcond */

template <class Buffer>
auto io_context::async_write(async_io::stream_socket_view socket,
                              Buffer&& buffer, int flags) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  return detail::async_write_sender(*this, socket, std::move(holder), flags);
}

template <class Buffer>
auto io_context::async_write(tcp_socket& socket, Buffer&& buffer, int flags) {
  return async_write(socket.view(), std::forward<Buffer>(buffer), flags);
}

}  // namespace bupp

#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_WRITE_OPERATION_H_
