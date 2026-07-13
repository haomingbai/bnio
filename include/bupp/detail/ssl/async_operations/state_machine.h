#pragma once
#ifndef BUPP_DETAIL_SSL_ASYNC_OPERATIONS_STATE_MACHINE_H_
#define BUPP_DETAIL_SSL_ASYNC_OPERATIONS_STATE_MACHINE_H_

#include <bupp/detail/ssl/async_operations/common.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/detail/operation_storage.hpp>
#include <bexec/receiver.hpp>
#include <bexec/scheduler.hpp>
#include <bexec/sender.hpp>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bupp {

/** @cond BUPP_DETAIL */
namespace detail {

enum class ssl_completion_kind {
  value,
  error,
  stopped,
};

enum class ssl_child_io {
  none,
  read,
  write,
};

enum class ssl_output_chunk_state {
  none,
  ready,
  error,
};

template <class Derived, class Scheduler, class NextLayer, class Receiver>
class ssl_async_operation_base {
 public:
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using receiver_type = std::remove_cvref_t<Receiver>;

  ssl_async_operation_base(Scheduler scheduler, ssl_stream<NextLayer>& stream,
                           Receiver receiver)
      : scheduler_(std::move(scheduler)),
        stream_(&stream),
        receiver_(std::move(receiver)) {}

  ssl_async_operation_base(const ssl_async_operation_base&) = delete;
  ssl_async_operation_base& operator=(const ssl_async_operation_base&) = delete;
  ssl_async_operation_base(ssl_async_operation_base&&) = delete;
  ssl_async_operation_base& operator=(ssl_async_operation_base&&) = delete;

  void start() noexcept {
    if (ssl_stop_requested(receiver_)) {
      post_complete_stopped();
      return;
    }

    static_cast<Derived*>(this)->on_start();
  }

 protected:
  class child_receiver {
   public:
    explicit child_receiver(ssl_async_operation_base& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] decltype(auto) get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::size_t bytes) noexcept {
      operation_->handle_transport_complete(bytes);
    }

    void set_error(std::error_code error) noexcept {
      operation_->post_complete_error(error);
    }

    void set_stopped() noexcept { operation_->post_complete_stopped(); }

   private:
    ssl_async_operation_base* operation_;
  };

  class post_receiver {
   public:
    explicit post_receiver(ssl_async_operation_base& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] decltype(auto) get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value() noexcept { operation_->deliver_terminal(); }

    void set_stopped() noexcept { operation_->deliver_terminal(); }

   private:
    ssl_async_operation_base* operation_;
  };

  using read_sender_type = decltype(ssl_make_transport_read_sender(
      std::declval<scheduler_type&>(), std::declval<ssl_stream<NextLayer>&>(),
      static_cast<void*>(nullptr), std::size_t{}));
  using write_sender_type = decltype(ssl_make_transport_write_sender(
      std::declval<scheduler_type&>(), std::declval<ssl_stream<NextLayer>&>(),
      static_cast<const void*>(nullptr), std::size_t{}));
  using post_sender_type =
      decltype(bexec::schedule(std::declval<scheduler_type&>()));
  using read_operation_type = decltype(bexec::connect(
      std::declval<read_sender_type>(), std::declval<child_receiver>()));
  using write_operation_type = decltype(bexec::connect(
      std::declval<write_sender_type>(), std::declval<child_receiver>()));
  using post_operation_type = decltype(bexec::connect(
      std::declval<post_sender_type>(), std::declval<post_receiver>()));
  using child_operations_type =
      bexec::detail::operation_storage<bexec::type_list<
          read_operation_type, write_operation_type, post_operation_type>>;

  void complete_value() noexcept {
    completion_ = ssl_completion_kind::value;
    child_ = ssl_child_io::none;
  }

  void complete_error(std::error_code error) noexcept {
    error_ = error;
    completion_ = ssl_completion_kind::error;
    child_ = ssl_child_io::none;
  }

  void complete_stopped() noexcept {
    completion_ = ssl_completion_kind::stopped;
    child_ = ssl_child_io::none;
  }

  void post_complete_value() noexcept {
    complete_value();
    submit_post();
  }

  void post_complete_error(std::error_code error) noexcept {
    complete_error(error);
    submit_post();
  }

  void post_complete_stopped() noexcept {
    complete_stopped();
    submit_post();
  }

  void flush_then(ssl_resume_action action) noexcept {
    after_flush_ = action;
    switch (load_output_chunk()) {
      case ssl_output_chunk_state::ready:
        submit_transport_write();
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
        after_read_ = action;
        flush_then(ssl_resume_action::transport_read);
        return;
      case SSL_ERROR_WANT_WRITE:
        flush_then(action);
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
    return ssl_bounded_int_size(size);
  }

  scheduler_type scheduler_;
  ssl_stream<NextLayer>* stream_;
  receiver_type receiver_;

 private:
  [[nodiscard]] ssl_output_chunk_state load_output_chunk() noexcept {
    char* data = nullptr;
    const int available = BIO_nread0(write_bio(*stream_), &data);
    if (available > 0) {
      transport_data_ = data;
      transport_size_ = static_cast<std::size_t>(available);
      return ssl_output_chunk_state::ready;
    }

    if (available < -1) {
      post_complete_error(last_ssl_error());
      return ssl_output_chunk_state::error;
    }

    transport_data_ = nullptr;
    transport_size_ = 0;
    return ssl_output_chunk_state::none;
  }

  void submit_transport_read() noexcept {
    char* data = nullptr;
    const int available = BIO_nwrite0(read_bio(*stream_), &data);
    if (available <= 0) {
      post_complete_error(last_ssl_error());
      return;
    }

    child_ = ssl_child_io::read;
    transport_data_ = data;
    transport_size_ = static_cast<std::size_t>(available);
    child_operation_.template emplace_from<read_operation_type>([this] {
      return bexec::connect(
          ssl_make_transport_read_sender(scheduler_, *stream_, transport_data_,
                                         transport_size_),
          child_receiver(*this));
    });
    child_operation_.start();
  }

  void submit_transport_write() noexcept {
    child_ = ssl_child_io::write;
    child_operation_.template emplace_from<write_operation_type>([this] {
      return bexec::connect(
          ssl_make_transport_write_sender(scheduler_, *stream_, transport_data_,
                                          transport_size_),
          child_receiver(*this));
    });
    child_operation_.start();
  }

  void submit_post() noexcept {
    child_operation_.template emplace_from<post_operation_type>([this] {
      return bexec::connect(bexec::schedule(scheduler_), post_receiver(*this));
    });
    child_operation_.start();
  }

  void handle_transport_complete(std::size_t result) noexcept {
    const ssl_child_io completed_child = child_;
    child_ = ssl_child_io::none;

    switch (completed_child) {
      case ssl_child_io::read:
        handle_read_complete(result);
        return;
      case ssl_child_io::write:
        handle_write_complete(result);
        return;
      case ssl_child_io::none:
        post_complete_error(std::make_error_code(std::errc::protocol_error));
        return;
    }
  }

  void handle_read_complete(std::size_t result) noexcept {
    if (result <= 0) {
      post_complete_error(std::make_error_code(std::errc::connection_reset));
      return;
    }

    char* data = nullptr;
    const int committed =
        BIO_nwrite(read_bio(*stream_), &data, bounded_int_size(result));
    if (committed != static_cast<int>(result)) {
      post_complete_error(last_ssl_error());
      return;
    }

    resume(after_read_);
  }

  void handle_write_complete(std::size_t result) noexcept {
    if (result <= 0) {
      post_complete_error(std::make_error_code(std::errc::connection_reset));
      return;
    }

    char* data = nullptr;
    const int consumed =
        BIO_nread(write_bio(*stream_), &data, bounded_int_size(result));
    if (consumed != static_cast<int>(result)) {
      post_complete_error(last_ssl_error());
      return;
    }

    switch (load_output_chunk()) {
      case ssl_output_chunk_state::ready:
        submit_transport_write();
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
    if (action == ssl_resume_action::transport_read) {
      submit_transport_read();
      return;
    }
    resume(action);
  }

  void resume(ssl_resume_action action) noexcept {
    static_cast<Derived*>(this)->resume(action);
  }

  void deliver_terminal() noexcept {
    switch (completion_) {
      case ssl_completion_kind::value:
        static_cast<Derived*>(this)->deliver_value();
        break;
      case ssl_completion_kind::error:
        bexec::set_error(std::move(receiver_), error_);
        break;
      case ssl_completion_kind::stopped:
        bexec::set_stopped(std::move(receiver_));
        break;
    }
  }

  child_operations_type child_operation_;
  std::error_code error_;
  char* transport_data_ = nullptr;
  std::size_t transport_size_ = 0;
  ssl_child_io child_ = ssl_child_io::none;
  ssl_completion_kind completion_ = ssl_completion_kind::value;
  ssl_resume_action after_read_ = ssl_resume_action::handshake;
  ssl_resume_action after_flush_ = ssl_resume_action::handshake;
};

}  // namespace detail
/** @endcond */

}  // namespace bupp

#endif  // BUPP_DETAIL_SSL_ASYNC_OPERATIONS_STATE_MACHINE_H_
