#pragma once
#ifndef BUPP_SSL_H_
#define BUPP_SSL_H_

#include <bupp/buffer.h>
#include <bupp/io_context.h>
#include <bupp/tcp.h>
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

/**
 * OpenSSL SSL_CTX method selector.
 */
enum class ssl_context_method {
  /**
   * Generic TLS method.
   */
  tls,

  /**
   * TLS client method.
   */
  tls_client,

  /**
   * TLS server method.
   */
  tls_server,
};

/**
 * Direction used when starting an SSL/TLS handshake.
 */
enum class ssl_handshake_type {
  /**
   * Start a client-side handshake.
   */
  client,

  /**
   * Start a server-side handshake.
   */
  server,
};

/**
 * Returns the error category used for OpenSSL library errors.
 */
[[nodiscard]] BUPP_EXPORT const std::error_category&
openssl_error_category() noexcept;

/**
 * Creates an error_code in the OpenSSL error category.
 */
[[nodiscard]] BUPP_EXPORT std::error_code make_openssl_error(
    unsigned long code) noexcept;

/**
 * RAII owner for an OpenSSL SSL_CTX object.
 *
 * ssl_context owns the native context and frees it on destruction. It is
 * move-only because the native SSL_CTX ownership is unique.
 */
class BUPP_EXPORT ssl_context {
 public:
  /**
   * Creates an SSL context for the selected TLS method.
   */
  explicit ssl_context(
      ssl_context_method method = ssl_context_method::tls) noexcept;

  /**
   * Frees the owned SSL_CTX, if any.
   */
  ~ssl_context() noexcept;

  /**
   * Copy construction is disabled because the context owns an SSL_CTX.
   */
  ssl_context(const ssl_context&) = delete;

  /**
   * Copy assignment is disabled because the context owns an SSL_CTX.
   */
  ssl_context& operator=(const ssl_context&) = delete;

  /**
   * Moves SSL_CTX ownership from another context.
   */
  ssl_context(ssl_context&& other) noexcept;

  /**
   * Frees the current SSL_CTX and moves ownership from another context.
   */
  ssl_context& operator=(ssl_context&& other) noexcept;

  /**
   * Returns the owned native SSL_CTX pointer, or nullptr when invalid.
   */
  [[nodiscard]] SSL_CTX* native_handle() const noexcept { return context_; }

  /**
   * Returns whether this context owns a native SSL_CTX.
   */
  [[nodiscard]] bool valid() const noexcept { return context_ != nullptr; }

  /**
   * Loads a certificate chain file into the context.
   */
  [[nodiscard]] std::error_code use_certificate_chain_file(
      const char* path) noexcept;

  /**
   * Loads a PEM private key file into the context.
   */
  [[nodiscard]] std::error_code use_private_key_file(const char* path) noexcept;

  /**
   * Checks whether the loaded private key matches the certificate.
   */
  [[nodiscard]] std::error_code check_private_key() noexcept;

  /**
   * Sets OpenSSL certificate verification flags for the context.
   */
  void set_verify_mode(int mode) noexcept;

 private:
  SSL_CTX* context_ = nullptr;
};

/**
 * RAII owner for an OpenSSL SSL object layered over a next stream.
 *
 * ssl_stream owns the SSL object and its memory BIOs. It also owns or stores
 * the supplied next layer by value. The stream is move-only because SSL
 * ownership is unique.
 */
template <class NextLayer = tcp_socket>
class ssl_stream {
 public:
  /**
   * Creates an SSL stream over a next layer using an existing context.
   */
  ssl_stream(NextLayer next_layer, ssl_context& context) noexcept
      : next_layer_(std::move(next_layer)) {
    reset(context);
  }

  /**
   * Frees the owned SSL object and its BIOs.
   */
  ~ssl_stream() noexcept {
    if (ssl_ != nullptr) {
      SSL_free(ssl_);
    }
  }

  /**
   * Copy construction is disabled because the stream owns an SSL object.
   */
  ssl_stream(const ssl_stream&) = delete;

  /**
   * Copy assignment is disabled because the stream owns an SSL object.
   */
  ssl_stream& operator=(const ssl_stream&) = delete;

  /**
   * Moves SSL, BIO, and next-layer ownership from another stream.
   */
  ssl_stream(ssl_stream&& other) noexcept
      : next_layer_(std::move(other.next_layer_)),
        ssl_(std::exchange(other.ssl_, nullptr)),
        read_bio_(std::exchange(other.read_bio_, nullptr)),
        write_bio_(std::exchange(other.write_bio_, nullptr)) {}

  /**
   * Frees the current SSL object and moves ownership from another stream.
   */
  ssl_stream& operator=(ssl_stream&& other) noexcept {
    if (this != &other) {
      if (ssl_ != nullptr) {
        SSL_free(ssl_);
      }
      next_layer_ = std::move(other.next_layer_);
      ssl_ = std::exchange(other.ssl_, nullptr);
      read_bio_ = std::exchange(other.read_bio_, nullptr);
      write_bio_ = std::exchange(other.write_bio_, nullptr);
    }
    return *this;
  }

  /**
   * Returns the next layer stored by this SSL stream.
   */
  [[nodiscard]] NextLayer& next_layer() noexcept { return next_layer_; }

  /**
   * Returns the next layer stored by this SSL stream.
   */
  [[nodiscard]] const NextLayer& next_layer() const noexcept {
    return next_layer_;
  }

  /**
   * Returns the lowest layer for asynchronous socket operations.
   */
  [[nodiscard]] NextLayer& lowest_layer() noexcept { return next_layer_; }

  /**
   * Returns the lowest layer for asynchronous socket operations.
   */
  [[nodiscard]] const NextLayer& lowest_layer() const noexcept {
    return next_layer_;
  }

  /**
   * Returns the owned native SSL pointer, or nullptr when invalid.
   */
  [[nodiscard]] SSL* native_handle() const noexcept { return ssl_; }

  /**
   * Returns the owned native SSL pointer, or nullptr when invalid.
   */
  [[nodiscard]] SSL* get_native_handle() const noexcept {
    return native_handle();
  }

  /**
   * Returns whether this stream owns a native SSL object.
   */
  [[nodiscard]] bool valid() const noexcept { return ssl_ != nullptr; }

  /**
   * Returns the memory BIO used for encrypted input.
   */
  [[nodiscard]] BIO* native_read_bio() const noexcept { return read_bio_; }

  /**
   * Returns the memory BIO used for encrypted output.
   */
  [[nodiscard]] BIO* native_write_bio() const noexcept { return write_bio_; }

 private:
  void reset(ssl_context& context) noexcept {
    ssl_ = SSL_new(context.native_handle());
    if (ssl_ == nullptr) {
      return;
    }

    read_bio_ = BIO_new(BIO_s_mem());
    write_bio_ = BIO_new(BIO_s_mem());
    if (read_bio_ == nullptr || write_bio_ == nullptr) {
      if (read_bio_ != nullptr) {
        BIO_free(read_bio_);
      }
      if (write_bio_ != nullptr) {
        BIO_free(write_bio_);
      }
      SSL_free(ssl_);
      ssl_ = nullptr;
      read_bio_ = nullptr;
      write_bio_ = nullptr;
      return;
    }

    BIO_set_mem_eof_return(read_bio_, -1);
    SSL_set_bio(ssl_, read_bio_, write_bio_);
    SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE |
                           SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  }

  NextLayer next_layer_;
  SSL* ssl_ = nullptr;
  BIO* read_bio_ = nullptr;
  BIO* write_bio_ = nullptr;
};

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

template <class Derived, class NextLayer, class Receiver>
class ssl_async_operation_base : public ssl_completion_base {
 public:
  ssl_async_operation_base(io_context& context, ssl_stream<NextLayer>& stream,
                           Receiver receiver)
      : context_(&context), stream_(&stream), receiver_(std::move(receiver)) {}

  [[nodiscard]] int prepare_for_submit() noexcept override {
    switch (child_) {
      case ssl_child_io::read:
        return context_->native_context().prepare(*this);
      case ssl_child_io::write:
        return context_->native_context().prepare(*this);
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
      context_->post(*this);
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
      context_->post(*this);
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
    context_->post(*this);
  }

  void post_complete_error(std::error_code error) noexcept {
    complete_error(error);
    context_->post(*this);
  }

  void post_complete_stopped() noexcept {
    complete_stopped();
    context_->post(*this);
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

  io_context* context_;
  ssl_stream<NextLayer>* stream_;
  std::remove_cvref_t<Receiver> receiver_;

 private:
  void submit_child(ssl_child_io child) noexcept {
    child_ = child;
    context_->submit_direct(*this);
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

template <class NextLayer, class Receiver>
class ssl_handshake_operation
    : public ssl_async_operation_base<
          ssl_handshake_operation<NextLayer, Receiver>, NextLayer, Receiver> {
 public:
  using base =
      ssl_async_operation_base<ssl_handshake_operation<NextLayer, Receiver>,
                               NextLayer, Receiver>;

  ssl_handshake_operation(io_context& context, ssl_stream<NextLayer>& stream,
                          ssl_handshake_type type, Receiver receiver)
      : base(context, stream, std::move(receiver)), type_(type) {}

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

template <class NextLayer, class Holder, class Receiver>
class ssl_receive_operation
    : public ssl_async_operation_base<
          ssl_receive_operation<NextLayer, Holder, Receiver>, NextLayer,
          Receiver> {
 public:
  using base = ssl_async_operation_base<
      ssl_receive_operation<NextLayer, Holder, Receiver>, NextLayer, Receiver>;

  ssl_receive_operation(io_context& context, ssl_stream<NextLayer>& stream,
                        Holder buffer, Receiver receiver)
      : base(context, stream, std::move(receiver)),
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

template <class NextLayer, class Holder, class Receiver>
class ssl_send_operation : public ssl_async_operation_base<
                               ssl_send_operation<NextLayer, Holder, Receiver>,
                               NextLayer, Receiver> {
 public:
  using base =
      ssl_async_operation_base<ssl_send_operation<NextLayer, Holder, Receiver>,
                               NextLayer, Receiver>;

  ssl_send_operation(io_context& context, ssl_stream<NextLayer>& stream,
                     Holder buffer, Receiver receiver)
      : base(context, stream, std::move(receiver)),
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

template <class NextLayer, class Receiver>
class ssl_shutdown_operation
    : public ssl_async_operation_base<
          ssl_shutdown_operation<NextLayer, Receiver>, NextLayer, Receiver> {
 public:
  using base =
      ssl_async_operation_base<ssl_shutdown_operation<NextLayer, Receiver>,
                               NextLayer, Receiver>;

  ssl_shutdown_operation(io_context& context, ssl_stream<NextLayer>& stream,
                         Receiver receiver)
      : base(context, stream, std::move(receiver)) {}

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

template <class NextLayer>
class ssl_handshake_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  ssl_handshake_sender(io_context& context, ssl_stream<NextLayer>& stream,
                       ssl_handshake_type type) noexcept
      : context_(&context), stream_(&stream), type_(type) {}

  template <class Receiver>
  auto connect(Receiver receiver) const {
    return ssl_handshake_operation<NextLayer, std::remove_cvref_t<Receiver>>(
        *context_, *stream_, type_, std::move(receiver));
  }

 private:
  io_context* context_;
  ssl_stream<NextLayer>* stream_;
  ssl_handshake_type type_;
};

template <class NextLayer, class Holder>
class ssl_receive_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  ssl_receive_sender(io_context& context, ssl_stream<NextLayer>& stream,
                     Holder buffer)
      : context_(&context), stream_(&stream), buffer_(std::move(buffer)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return ssl_receive_operation<NextLayer, Holder,
                                 std::remove_cvref_t<Receiver>>(
        *context_, *stream_, std::move(buffer_), std::move(receiver));
  }

 private:
  io_context* context_;
  ssl_stream<NextLayer>* stream_;
  Holder buffer_;
};

template <class NextLayer, class Holder>
class ssl_send_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  ssl_send_sender(io_context& context, ssl_stream<NextLayer>& stream,
                  Holder buffer)
      : context_(&context), stream_(&stream), buffer_(std::move(buffer)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return ssl_send_operation<NextLayer, Holder, std::remove_cvref_t<Receiver>>(
        *context_, *stream_, std::move(buffer_), std::move(receiver));
  }

 private:
  io_context* context_;
  ssl_stream<NextLayer>* stream_;
  Holder buffer_;
};

template <class NextLayer>
class ssl_shutdown_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  ssl_shutdown_sender(io_context& context, ssl_stream<NextLayer>& stream)
      : context_(&context), stream_(&stream) {}

  template <class Receiver>
  auto connect(Receiver receiver) const {
    return ssl_shutdown_operation<NextLayer, std::remove_cvref_t<Receiver>>(
        *context_, *stream_, std::move(receiver));
  }

 private:
  io_context* context_;
  ssl_stream<NextLayer>* stream_;
};

}  // namespace detail
/** @endcond */

/**
 * Creates a sender that performs an SSL/TLS handshake on a stream.
 */
template <class NextLayer>
auto io_context::async_handshake(ssl_stream<NextLayer>& stream,
                                 ssl_handshake_type type) {
  return detail::ssl_handshake_sender<NextLayer>(*this, stream, type);
}

/**
 * Creates a direct-submission sender that performs an SSL/TLS handshake.
 */
template <class NextLayer>
auto io_context::async_handshake_direct(ssl_stream<NextLayer>& stream,
                                        ssl_handshake_type type) {
  return detail::ssl_handshake_sender<NextLayer>(*this, stream, type);
}

/**
 * Creates a sender that receives decrypted bytes from an SSL stream.
 */
template <class NextLayer, class Buffer>
auto io_context::async_receive(ssl_stream<NextLayer>& stream, Buffer&& buffer,
                               int) {
  auto holder =
      detail::make_mutable_buffer_holder(std::forward<Buffer>(buffer));
  using holder_type = decltype(holder);
  return detail::ssl_receive_sender<NextLayer, holder_type>(*this, stream,
                                                            std::move(holder));
}

/**
 * Creates a direct-submission sender that receives decrypted bytes.
 */
template <class NextLayer, class Buffer>
auto io_context::async_receive_direct(ssl_stream<NextLayer>& stream,
                                      Buffer&& buffer, int flags) {
  return async_receive(stream, std::forward<Buffer>(buffer), flags);
}

/**
 * Creates a sender that sends encrypted bytes through an SSL stream.
 */
template <class NextLayer, class Buffer>
auto io_context::async_send(ssl_stream<NextLayer>& stream, Buffer&& buffer,
                            int) {
  auto holder = detail::make_const_buffer_holder(std::forward<Buffer>(buffer));
  using holder_type = decltype(holder);
  return detail::ssl_send_sender<NextLayer, holder_type>(*this, stream,
                                                         std::move(holder));
}

/**
 * Creates a direct-submission sender that sends encrypted bytes.
 */
template <class NextLayer, class Buffer>
auto io_context::async_send_direct(ssl_stream<NextLayer>& stream,
                                   Buffer&& buffer, int flags) {
  return async_send(stream, std::forward<Buffer>(buffer), flags);
}

/**
 * Creates a sender that shuts down an SSL stream.
 */
template <class NextLayer>
auto io_context::async_shutdown(ssl_stream<NextLayer>& stream) {
  return detail::ssl_shutdown_sender<NextLayer>(*this, stream);
}

/**
 * Creates a direct-submission sender that shuts down an SSL stream.
 */
template <class NextLayer>
auto io_context::async_shutdown_direct(ssl_stream<NextLayer>& stream) {
  return detail::ssl_shutdown_sender<NextLayer>(*this, stream);
}

/**
 * Customization point object for Provider::async_handshake.
 */
struct async_handshake_t {
  /**
   * Invokes async_handshake on a provider.
   */
  template <class Provider, class Stream>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      ssl_handshake_type type) const
      noexcept(noexcept(std::forward<Provider>(provider).async_handshake(
          std::forward<Stream>(stream), type))) {
    return std::forward<Provider>(provider).async_handshake(
        std::forward<Stream>(stream), type);
  }
};

/**
 * Customization point object for Provider::async_handshake_direct.
 */
struct async_handshake_direct_t {
  /**
   * Invokes async_handshake_direct on a provider.
   */
  template <class Provider, class Stream>
  constexpr decltype(auto) operator()(Provider&& provider, Stream&& stream,
                                      ssl_handshake_type type) const
      noexcept(noexcept(std::forward<Provider>(provider).async_handshake_direct(
          std::forward<Stream>(stream), type))) {
    return std::forward<Provider>(provider).async_handshake_direct(
        std::forward<Stream>(stream), type);
  }
};

/**
 * Customization point object for Provider::async_shutdown.
 */
struct async_shutdown_t {
  /**
   * Invokes async_shutdown on a provider.
   */
  template <class Provider, class Stream>
  constexpr decltype(auto) operator()(Provider&& provider,
                                      Stream&& stream) const
      noexcept(noexcept(std::forward<Provider>(provider).async_shutdown(
          std::forward<Stream>(stream)))) {
    return std::forward<Provider>(provider).async_shutdown(
        std::forward<Stream>(stream));
  }
};

/**
 * Customization point object for Provider::async_shutdown_direct.
 */
struct async_shutdown_direct_t {
  /**
   * Invokes async_shutdown_direct on a provider.
   */
  template <class Provider, class Stream>
  constexpr decltype(auto) operator()(Provider&& provider,
                                      Stream&& stream) const
      noexcept(noexcept(std::forward<Provider>(provider).async_shutdown_direct(
          std::forward<Stream>(stream)))) {
    return std::forward<Provider>(provider).async_shutdown_direct(
        std::forward<Stream>(stream));
  }
};

/**
 * Customization point object instance for async_handshake.
 */
inline constexpr async_handshake_t async_handshake{};

/**
 * Customization point object instance for async_handshake_direct.
 */
inline constexpr async_handshake_direct_t async_handshake_direct{};

/**
 * Customization point object instance for async_shutdown.
 */
inline constexpr async_shutdown_t async_shutdown{};

/**
 * Customization point object instance for async_shutdown_direct.
 */
inline constexpr async_shutdown_direct_t async_shutdown_direct{};

}  // namespace bupp

#endif  // BUPP_SSL_H_
