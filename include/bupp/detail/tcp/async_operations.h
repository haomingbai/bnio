#pragma once
#ifndef BUPP_DETAIL_TCP_ASYNC_OPERATIONS_H_
#define BUPP_DETAIL_TCP_ASYNC_OPERATIONS_H_

#include <bupp/async_io/socket_view.h>
#include <bupp/buffer.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/detail/manual_lifetime.hpp>
#include <bexec/receiver.hpp>
#include <bexec/sender.hpp>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bupp {

class tcp_socket;

/** @cond BUPP_DETAIL */
namespace detail {

template <class Scheduler, class Holder, bool DirectSubmit, class Receiver>
class tcp_read_operation {
 public:
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using receiver_type = std::remove_cvref_t<Receiver>;

  static auto make_child_sender(scheduler_type& scheduler,
                                async_io::stream_socket_view socket,
                                async_io::buffer_view buffer, int flags) {
    if constexpr (DirectSubmit) {
      return scheduler.async_read_direct(socket, bupp::buffer(buffer), flags);
    } else {
      return scheduler.async_read(socket, bupp::buffer(buffer), flags);
    }
  }

  class child_receiver {
   public:
    explicit child_receiver(tcp_read_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] decltype(auto) get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::size_t size) noexcept {
      operation_->holder_.commit(size);
      bexec::set_value(std::move(operation_->receiver_), size);
    }

    void set_error(std::error_code error) noexcept {
      bexec::set_error(std::move(operation_->receiver_), error);
    }

    void set_stopped() noexcept {
      bexec::set_stopped(std::move(operation_->receiver_));
    }

   private:
    tcp_read_operation* operation_;
  };

  using child_sender_type =
      decltype(make_child_sender(std::declval<scheduler_type&>(),
                                 std::declval<async_io::stream_socket_view>(),
                                 std::declval<Holder&>().view(), int{}));
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  tcp_read_operation(scheduler_type scheduler,
                     async_io::stream_socket_view socket, Holder holder,
                     int flags, Receiver receiver)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        flags_(flags),
        receiver_(std::move(receiver)) {
    child_operation_.emplace_from([this] {
      return bexec::connect(
          make_child_sender(scheduler_, socket_, holder_.view(), flags_),
          child_receiver(*this));
    });
  }

  tcp_read_operation(const tcp_read_operation&) = delete;
  tcp_read_operation& operator=(const tcp_read_operation&) = delete;
  tcp_read_operation(tcp_read_operation&&) = delete;
  tcp_read_operation& operator=(tcp_read_operation&&) = delete;

  void start() noexcept { bexec::start(*child_operation_); }

 private:
  scheduler_type scheduler_;
  async_io::stream_socket_view socket_;
  Holder holder_;
  int flags_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

template <class Scheduler, class Holder, bool DirectSubmit>
class tcp_read_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  tcp_read_sender(Scheduler scheduler, async_io::stream_socket_view socket,
                  Holder holder, int flags)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        flags_(flags) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return tcp_read_operation<Scheduler, Holder, DirectSubmit,
                              std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), socket_, std::move(holder_), flags_,
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  async_io::stream_socket_view socket_;
  Holder holder_;
  int flags_;
};

template <class Scheduler, class Holder, bool DirectSubmit, class Receiver>
class tcp_write_operation {
 public:
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using receiver_type = std::remove_cvref_t<Receiver>;

  static auto make_child_sender(scheduler_type& scheduler,
                                async_io::stream_socket_view socket,
                                const_buffer buffer, int flags) {
    if constexpr (DirectSubmit) {
      return scheduler.async_write_direct(socket, buffer, flags);
    } else {
      return scheduler.async_write(socket, buffer, flags);
    }
  }

  class child_receiver {
   public:
    explicit child_receiver(tcp_write_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] decltype(auto) get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::size_t size) noexcept {
      bexec::set_value(std::move(operation_->receiver_), size);
    }

    void set_error(std::error_code error) noexcept {
      bexec::set_error(std::move(operation_->receiver_), error);
    }

    void set_stopped() noexcept {
      bexec::set_stopped(std::move(operation_->receiver_));
    }

   private:
    tcp_write_operation* operation_;
  };

  using child_sender_type =
      decltype(make_child_sender(std::declval<scheduler_type&>(),
                                 std::declval<async_io::stream_socket_view>(),
                                 const_buffer(std::declval<Holder&>().data(),
                                              std::declval<Holder&>().size()),
                                 int{}));
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  tcp_write_operation(scheduler_type scheduler,
                      async_io::stream_socket_view socket, Holder holder,
                      int flags, Receiver receiver)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        flags_(flags),
        receiver_(std::move(receiver)) {
    child_operation_.emplace_from([this] {
      return bexec::connect(
          make_child_sender(scheduler_, socket_,
                            const_buffer(holder_.data(), holder_.size()),
                            flags_),
          child_receiver(*this));
    });
  }

  tcp_write_operation(const tcp_write_operation&) = delete;
  tcp_write_operation& operator=(const tcp_write_operation&) = delete;
  tcp_write_operation(tcp_write_operation&&) = delete;
  tcp_write_operation& operator=(tcp_write_operation&&) = delete;

  void start() noexcept { bexec::start(*child_operation_); }

 private:
  scheduler_type scheduler_;
  async_io::stream_socket_view socket_;
  Holder holder_;
  int flags_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

template <class Scheduler, class Holder, bool DirectSubmit>
class tcp_write_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  tcp_write_sender(Scheduler scheduler, async_io::stream_socket_view socket,
                   Holder holder, int flags)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        flags_(flags) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return tcp_write_operation<Scheduler, Holder, DirectSubmit,
                               std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), socket_, std::move(holder_), flags_,
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  async_io::stream_socket_view socket_;
  Holder holder_;
  int flags_;
};

template <class Scheduler, bool DirectSubmit, class Receiver>
class tcp_accept_operation {
 public:
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using receiver_type = std::remove_cvref_t<Receiver>;

  static auto make_child_sender(scheduler_type& scheduler,
                                async_io::listening_socket_view socket,
                                int flags) {
    if constexpr (DirectSubmit) {
      return scheduler.async_accept_direct(socket, flags);
    } else {
      return scheduler.async_accept(socket, flags);
    }
  }

  class child_receiver {
   public:
    explicit child_receiver(tcp_accept_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] decltype(auto) get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(int fd) noexcept;

    void set_error(std::error_code error) noexcept {
      bexec::set_error(std::move(operation_->receiver_), error);
    }

    void set_stopped() noexcept {
      bexec::set_stopped(std::move(operation_->receiver_));
    }

   private:
    tcp_accept_operation* operation_;
  };

  using child_sender_type = decltype(make_child_sender(
      std::declval<scheduler_type&>(),
      std::declval<async_io::listening_socket_view>(), int{}));
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  tcp_accept_operation(scheduler_type scheduler,
                       async_io::listening_socket_view socket, int flags,
                       Receiver receiver)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        flags_(flags),
        receiver_(std::move(receiver)) {
    child_operation_.emplace_from([this] {
      return bexec::connect(make_child_sender(scheduler_, socket_, flags_),
                            child_receiver(*this));
    });
  }

  tcp_accept_operation(const tcp_accept_operation&) = delete;
  tcp_accept_operation& operator=(const tcp_accept_operation&) = delete;
  tcp_accept_operation(tcp_accept_operation&&) = delete;
  tcp_accept_operation& operator=(tcp_accept_operation&&) = delete;

  void start() noexcept { bexec::start(*child_operation_); }

 private:
  scheduler_type scheduler_;
  async_io::listening_socket_view socket_;
  int flags_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

template <class Scheduler, bool DirectSubmit>
class tcp_accept_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(tcp_socket),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  tcp_accept_sender(Scheduler scheduler, async_io::listening_socket_view socket,
                    int flags)
      : scheduler_(std::move(scheduler)), socket_(socket), flags_(flags) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return tcp_accept_operation<Scheduler, DirectSubmit,
                                std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), socket_, flags_, std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  async_io::listening_socket_view socket_;
  int flags_;
};

}  // namespace detail
/** @endcond */

}  // namespace bupp

#endif  // BUPP_DETAIL_TCP_ASYNC_OPERATIONS_H_
