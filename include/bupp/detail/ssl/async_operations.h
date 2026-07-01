#pragma once
#ifndef BUPP_DETAIL_SSL_ASYNC_OPERATIONS_H_
#define BUPP_DETAIL_SSL_ASYNC_OPERATIONS_H_

#include <bupp/base/linux/submission_queue_entry.h>
#include <bupp/buffer.h>
#include <bupp/io_context.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>

#include <array>
#include <bexec/completion_signatures.hpp>
#include <bexec/receiver.hpp>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bupp {

/** @cond BUPP_DETAIL */
namespace detail {

template <class NextLayer>
[[nodiscard]] BIO* read_bio(ssl_stream<NextLayer>& stream) noexcept {
  return stream.native_read_bio();
}

template <class NextLayer>
[[nodiscard]] BIO* write_bio(ssl_stream<NextLayer>& stream) noexcept {
  return stream.native_write_bio();
}

[[nodiscard]] inline std::error_code errno_error(int value) noexcept {
  return std::error_code(value, std::generic_category());
}

[[nodiscard]] inline std::error_code last_ssl_error() noexcept {
  const unsigned long error = ERR_get_error();
  if (error == 0) {
    return std::make_error_code(std::errc::protocol_error);
  }
  return make_openssl_error(error);
}

class ssl_completion_base : public io_context::operation_base {
 public:
  [[nodiscard]] int prepare_for_submit() noexcept override { return -EINVAL; }

  void complete_submit_error(int result) noexcept override {
    error_ = std::error_code(-result, std::generic_category());
    completion_ = completion_kind::error;
  }

 protected:
  enum class completion_kind {
    value,
    error,
    stopped,
  };

  completion_kind completion_ = completion_kind::value;
  std::error_code error_;
};

enum class ssl_child_io {
  none,
  read,
  write,
};

enum class ssl_resume_action {
  handshake,
  receive,
  send,
  shutdown,
  read,
  finish,
};

enum class ssl_output_chunk_state {
  none,
  ready,
  error,
};

template <class Derived, class Scheduler, class NextLayer, class Receiver>
class ssl_async_operation_base : public ssl_completion_base {
 public:
  ssl_async_operation_base(Scheduler scheduler, ssl_stream<NextLayer>& stream,
                           Receiver receiver)
      : scheduler_(std::move(scheduler)),
        stream_(&stream),
        receiver_(std::move(receiver)) {}

  [[nodiscard]] int prepare_for_submit() noexcept override {
    switch (child_) {
      case ssl_child_io::read:
        return scheduler_.native_context().prepare(*this);
      case ssl_child_io::write:
        return scheduler_.native_context().prepare(*this);
      case ssl_child_io::none:
        return -EINVAL;
    }
    return -EINVAL;
  }

  void complete_submit_error(int result) noexcept override {
    child_ = ssl_child_io::none;
    this->error_ = std::error_code(-result, std::generic_category());
    this->completion_ = completion_kind::error;
  }

  void start() noexcept {
    if (stop_requested(receiver_)) {
      complete_stopped();
      scheduler_.post(*this);
      return;
    }

    static_cast<Derived*>(this)->on_start();
  }

  void execute() noexcept override {
    if (child_ == ssl_child_io::none) {
      deliver_terminal();
      return;
    }

    const ssl_child_io completed_child = child_;
    child_ = ssl_child_io::none;

    if (this->result < 0) {
      complete_error(std::error_code(-this->result, std::generic_category()));
      scheduler_.post(*this);
      return;
    }

    if (completed_child == ssl_child_io::read) {
      handle_read_complete(this->result);
    } else {
      handle_write_complete(this->result);
    }
  }

 protected:
  void complete_value() noexcept {
    this->completion_ = completion_kind::value;
    child_ = ssl_child_io::none;
  }

  void complete_error(std::error_code error) noexcept {
    this->error_ = error;
    this->completion_ = completion_kind::error;
    child_ = ssl_child_io::none;
  }

  void complete_stopped() noexcept {
    this->completion_ = completion_kind::stopped;
    child_ = ssl_child_io::none;
  }

  void post_complete_value() noexcept {
    complete_value();
    scheduler_.post(*this);
  }

  void post_complete_error(std::error_code error) noexcept {
    complete_error(error);
    scheduler_.post(*this);
  }

  void post_complete_stopped() noexcept {
    complete_stopped();
    scheduler_.post(*this);
  }

  void wait_read_then(ssl_resume_action action) noexcept {
    after_read_ = action;
    flush_then(ssl_resume_action::read);
  }

  void wait_write_then(ssl_resume_action action) noexcept {
    flush_then(action);
  }

  void flush_then(ssl_resume_action action) noexcept {
    after_flush_ = action;
    switch (load_output_chunk()) {
      case ssl_output_chunk_state::ready:
        submit_child(ssl_child_io::write);
        return;
      case ssl_output_chunk_state::none:
        resume_after_flush();
        return;
      case ssl_output_chunk_state::error:
        return;
    }
  }

  void handle_ssl_error(int ssl_result, ssl_resume_action action) noexcept {
    const int error = SSL_get_error(stream_->native_handle(), ssl_result);
    switch (error) {
      case SSL_ERROR_WANT_READ:
        wait_read_then(action);
        return;
      case SSL_ERROR_WANT_WRITE:
        wait_write_then(action);
        return;
      case SSL_ERROR_ZERO_RETURN:
        post_complete_error(std::make_error_code(std::errc::connection_reset));
        return;
      default:
        post_complete_error(last_ssl_error());
        return;
    }
  }

  [[nodiscard]] int bounded_int_size(std::size_t size) const noexcept {
    constexpr auto max_int =
        static_cast<std::size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(size > max_int ? max_int : size);
  }

  std::remove_cvref_t<Scheduler> scheduler_;
  ssl_stream<NextLayer>* stream_;
  std::remove_cvref_t<Receiver> receiver_;

 private:
  void submit_child(ssl_child_io child) noexcept {
    child_ = child;
    scheduler_.submit_direct(*this);
  }

  [[nodiscard]] ssl_output_chunk_state load_output_chunk() noexcept {
    BIO* output = write_bio(*stream_);
    if (BIO_pending(output) <= 0) {
      output_offset_ = 0;
      output_size_ = 0;
      return ssl_output_chunk_state::none;
    }

    const int result =
        BIO_read(output, output_.data(), static_cast<int>(output_.size()));
    if (result <= 0) {
      post_complete_error(last_ssl_error());
      return ssl_output_chunk_state::error;
    }

    output_offset_ = 0;
    output_size_ = static_cast<std::size_t>(result);
    return ssl_output_chunk_state::ready;
  }

  void handle_read_complete(int result) noexcept {
    if (result <= 0) {
      post_complete_error(std::make_error_code(std::errc::connection_reset));
      return;
    }

    const int written = BIO_write(read_bio(*stream_), input_.data(), result);
    if (written != result) {
      post_complete_error(last_ssl_error());
      return;
    }

    resume(after_read_);
  }

  void handle_write_complete(int result) noexcept {
    if (result <= 0) {
      post_complete_error(std::make_error_code(std::errc::connection_reset));
      return;
    }

    output_offset_ += static_cast<std::size_t>(result);
    if (output_offset_ < output_size_) {
      submit_child(ssl_child_io::write);
      return;
    }

    switch (load_output_chunk()) {
      case ssl_output_chunk_state::ready:
        submit_child(ssl_child_io::write);
        return;
      case ssl_output_chunk_state::none:
        resume_after_flush();
        return;
      case ssl_output_chunk_state::error:
        return;
    }
  }

  void resume_after_flush() noexcept {
    const ssl_resume_action action = after_flush_;
    if (action == ssl_resume_action::read) {
      submit_child(ssl_child_io::read);
      return;
    }
    resume(action);
  }

  void resume(ssl_resume_action action) noexcept {
    static_cast<Derived*>(this)->resume(action);
  }

  void deliver_terminal() noexcept {
    switch (this->completion_) {
      case completion_kind::value:
        static_cast<Derived*>(this)->deliver_value();
        break;
      case completion_kind::error:
        bexec::set_error(std::move(receiver_), this->error_);
        break;
      case completion_kind::stopped:
        bexec::set_stopped(std::move(receiver_));
        break;
    }
  }

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    switch (child_) {
      case ssl_child_io::read:
        sqe.prep_recv(stream_->lowest_layer().native_handle(), input_.data(),
                      input_.size(), 0);
        break;
      case ssl_child_io::write:
        sqe.prep_send(stream_->lowest_layer().native_handle(),
                      output_.data() + output_offset_,
                      output_size_ - output_offset_, MSG_NOSIGNAL);
        break;
      case ssl_child_io::none:
        sqe.prep_nop();
        break;
    }
  }

  friend class async_io::linux_native::io_uring_context;

  std::array<unsigned char, 16 * 1024> input_{};
  std::array<unsigned char, 16 * 1024> output_{};
  std::size_t output_offset_ = 0;
  std::size_t output_size_ = 0;
  ssl_child_io child_ = ssl_child_io::none;
  ssl_resume_action after_read_ = ssl_resume_action::handshake;
  ssl_resume_action after_flush_ = ssl_resume_action::handshake;
};

template <class Scheduler, class NextLayer, class Receiver>
class ssl_handshake_operation
    : public ssl_async_operation_base<
          ssl_handshake_operation<Scheduler, NextLayer, Receiver>, Scheduler,
          NextLayer, Receiver> {
 public:
  using base = ssl_async_operation_base<
      ssl_handshake_operation<Scheduler, NextLayer, Receiver>, Scheduler,
      NextLayer, Receiver>;

  ssl_handshake_operation(Scheduler scheduler, ssl_stream<NextLayer>& stream,
                          ssl_handshake_type type, Receiver receiver)
      : base(std::move(scheduler), stream, std::move(receiver)), type_(type) {}

  void on_start() noexcept {
    if (!this->stream_->valid()) {
      this->post_complete_error(last_ssl_error());
      return;
    }

    if (type_ == ssl_handshake_type::client) {
      SSL_set_connect_state(this->stream_->native_handle());
    } else {
      SSL_set_accept_state(this->stream_->native_handle());
    }
    run_handshake();
  }

  void resume(ssl_resume_action action) noexcept {
    if (action == ssl_resume_action::finish) {
      this->post_complete_value();
      return;
    }
    run_handshake();
  }

  void deliver_value() noexcept {
    bexec::set_value(std::move(this->receiver_));
  }

 private:
  void run_handshake() noexcept {
    const int result = SSL_do_handshake(this->stream_->native_handle());
    if (result == 1) {
      this->flush_then(ssl_resume_action::finish);
      return;
    }
    this->handle_ssl_error(result, ssl_resume_action::handshake);
  }

  ssl_handshake_type type_;
};

template <class Scheduler, class NextLayer, class Holder, class Receiver>
class ssl_receive_operation
    : public ssl_async_operation_base<
          ssl_receive_operation<Scheduler, NextLayer, Holder, Receiver>,
          Scheduler, NextLayer, Receiver> {
 public:
  using base = ssl_async_operation_base<
      ssl_receive_operation<Scheduler, NextLayer, Holder, Receiver>, Scheduler,
      NextLayer, Receiver>;

  ssl_receive_operation(Scheduler scheduler, ssl_stream<NextLayer>& stream,
                        Holder buffer, Receiver receiver)
      : base(std::move(scheduler), stream, std::move(receiver)),
        buffer_(std::move(buffer)) {}

  void on_start() noexcept { run_receive(); }

  void resume(ssl_resume_action action) noexcept {
    if (action == ssl_resume_action::finish) {
      this->post_complete_value();
      return;
    }
    run_receive();
  }

  void deliver_value() noexcept {
    buffer_.commit(bytes_);
    bexec::set_value(std::move(this->receiver_), bytes_);
  }

 private:
  void run_receive() noexcept {
    async_io::buffer_view view = buffer_.view();
    const int result = SSL_read(this->stream_->native_handle(), view.data,
                                this->bounded_int_size(view.size));
    if (result > 0) {
      bytes_ = static_cast<std::size_t>(result);
      this->flush_then(ssl_resume_action::finish);
      return;
    }
    this->handle_ssl_error(result, ssl_resume_action::receive);
  }

  Holder buffer_;
  std::size_t bytes_ = 0;
};

template <class Scheduler, class NextLayer, class Holder, class Receiver>
class ssl_send_operation
    : public ssl_async_operation_base<
          ssl_send_operation<Scheduler, NextLayer, Holder, Receiver>, Scheduler,
          NextLayer, Receiver> {
 public:
  using base = ssl_async_operation_base<
      ssl_send_operation<Scheduler, NextLayer, Holder, Receiver>, Scheduler,
      NextLayer, Receiver>;

  ssl_send_operation(Scheduler scheduler, ssl_stream<NextLayer>& stream,
                     Holder buffer, Receiver receiver)
      : base(std::move(scheduler), stream, std::move(receiver)),
        buffer_(std::move(buffer)) {}

  void on_start() noexcept { run_send(); }

  void resume(ssl_resume_action action) noexcept {
    if (action == ssl_resume_action::finish) {
      this->post_complete_value();
      return;
    }
    run_send();
  }

  void deliver_value() noexcept {
    bexec::set_value(std::move(this->receiver_), bytes_);
  }

 private:
  void run_send() noexcept {
    const int result = SSL_write(this->stream_->native_handle(), buffer_.data(),
                                 this->bounded_int_size(buffer_.size()));
    if (result > 0) {
      bytes_ = static_cast<std::size_t>(result);
      this->flush_then(ssl_resume_action::finish);
      return;
    }
    this->handle_ssl_error(result, ssl_resume_action::send);
  }

  Holder buffer_;
  std::size_t bytes_ = 0;
};

template <class Scheduler, class NextLayer, class Receiver>
class ssl_shutdown_operation
    : public ssl_async_operation_base<
          ssl_shutdown_operation<Scheduler, NextLayer, Receiver>, Scheduler,
          NextLayer, Receiver> {
 public:
  using base = ssl_async_operation_base<
      ssl_shutdown_operation<Scheduler, NextLayer, Receiver>, Scheduler,
      NextLayer, Receiver>;

  ssl_shutdown_operation(Scheduler scheduler, ssl_stream<NextLayer>& stream,
                         Receiver receiver)
      : base(std::move(scheduler), stream, std::move(receiver)) {}

  void on_start() noexcept { run_shutdown(); }

  void resume(ssl_resume_action action) noexcept {
    if (action == ssl_resume_action::finish) {
      this->post_complete_value();
      return;
    }
    run_shutdown();
  }

  void deliver_value() noexcept {
    bexec::set_value(std::move(this->receiver_));
  }

 private:
  void run_shutdown() noexcept {
    const int result = SSL_shutdown(this->stream_->native_handle());
    if (result == 1) {
      this->flush_then(ssl_resume_action::finish);
      return;
    }
    if (result == 0) {
      this->flush_then(ssl_resume_action::shutdown);
      return;
    }
    this->handle_ssl_error(result, ssl_resume_action::shutdown);
  }
};

template <class Scheduler, class NextLayer>
class ssl_handshake_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  ssl_handshake_sender(Scheduler scheduler, ssl_stream<NextLayer>& stream,
                       ssl_handshake_type type) noexcept
      : scheduler_(std::move(scheduler)), stream_(&stream), type_(type) {}

  template <class Receiver>
  auto connect(Receiver receiver) const {
    return ssl_handshake_operation<Scheduler, NextLayer,
                                   std::remove_cvref_t<Receiver>>(
        scheduler_, *stream_, type_, std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  ssl_stream<NextLayer>* stream_;
  ssl_handshake_type type_;
};

template <class Scheduler, class NextLayer, class Holder>
class ssl_receive_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  ssl_receive_sender(Scheduler scheduler, ssl_stream<NextLayer>& stream,
                     Holder buffer)
      : scheduler_(std::move(scheduler)),
        stream_(&stream),
        buffer_(std::move(buffer)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return ssl_receive_operation<Scheduler, NextLayer, Holder,
                                 std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), *stream_, std::move(buffer_),
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  ssl_stream<NextLayer>* stream_;
  Holder buffer_;
};

template <class Scheduler, class NextLayer, class Holder>
class ssl_send_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  ssl_send_sender(Scheduler scheduler, ssl_stream<NextLayer>& stream,
                  Holder buffer)
      : scheduler_(std::move(scheduler)),
        stream_(&stream),
        buffer_(std::move(buffer)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return ssl_send_operation<Scheduler, NextLayer, Holder,
                              std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), *stream_, std::move(buffer_),
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  ssl_stream<NextLayer>* stream_;
  Holder buffer_;
};

template <class Scheduler, class NextLayer>
class ssl_shutdown_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  ssl_shutdown_sender(Scheduler scheduler, ssl_stream<NextLayer>& stream)
      : scheduler_(std::move(scheduler)), stream_(&stream) {}

  template <class Receiver>
  auto connect(Receiver receiver) const {
    return ssl_shutdown_operation<Scheduler, NextLayer,
                                  std::remove_cvref_t<Receiver>>(
        scheduler_, *stream_, std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  ssl_stream<NextLayer>* stream_;
};

}  // namespace detail
/** @endcond */

}  // namespace bupp

#endif  // BUPP_DETAIL_SSL_ASYNC_OPERATIONS_H_
