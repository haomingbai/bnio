#pragma once
#ifndef BUPP_DETAIL_TCP_ASYNC_OPERATIONS_H_
#define BUPP_DETAIL_TCP_ASYNC_OPERATIONS_H_

#include <bupp/async_io/socket_view.h>
#include <bupp/buffer.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/detail/manual_lifetime.hpp>
#include <bexec/operation_state.hpp>
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

template <class Scheduler, class Holder, bool Direct, class Receiver>
class tcp_receive_operation {
 public:
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using receiver_type = std::remove_cvref_t<Receiver>;

  static auto make_child_sender(scheduler_type& scheduler,
                                async_io::stream_socket_view socket,
                                async_io::buffer_view buffer, int flags) {
    if constexpr (Direct) {
      return scheduler.async_receive_direct(socket, bupp::buffer(buffer),
                                            flags);
    } else {
      return scheduler.async_receive(socket, bupp::buffer(buffer), flags);
    }
  }

  class child_receiver {
   public:
    explicit child_receiver(tcp_receive_operation& operation) noexcept
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
    tcp_receive_operation* operation_;
  };

  using child_sender_type =
      decltype(make_child_sender(std::declval<scheduler_type&>(),
                                 std::declval<async_io::stream_socket_view>(),
                                 std::declval<Holder&>().view(), int{}));
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  tcp_receive_operation(scheduler_type scheduler,
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

  tcp_receive_operation(const tcp_receive_operation&) = delete;
  tcp_receive_operation& operator=(const tcp_receive_operation&) = delete;
  tcp_receive_operation(tcp_receive_operation&&) = delete;
  tcp_receive_operation& operator=(tcp_receive_operation&&) = delete;

  void start() noexcept { bexec::start(*child_operation_); }

 private:
  scheduler_type scheduler_;
  async_io::stream_socket_view socket_;
  Holder holder_;
  int flags_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

template <class Scheduler, class Holder, bool Direct>
class tcp_receive_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  tcp_receive_sender(Scheduler scheduler, async_io::stream_socket_view socket,
                     Holder holder, int flags)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        flags_(flags) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return tcp_receive_operation<Scheduler, Holder, Direct,
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

template <class Scheduler, class Holder, bool Direct, class Receiver>
class tcp_send_operation {
 public:
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using receiver_type = std::remove_cvref_t<Receiver>;

  static auto make_child_sender(scheduler_type& scheduler,
                                async_io::stream_socket_view socket,
                                const_buffer buffer, int flags) {
    if constexpr (Direct) {
      return scheduler.async_send_direct(socket, buffer, flags);
    } else {
      return scheduler.async_send(socket, buffer, flags);
    }
  }

  class child_receiver {
   public:
    explicit child_receiver(tcp_send_operation& operation) noexcept
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
    tcp_send_operation* operation_;
  };

  using child_sender_type =
      decltype(make_child_sender(std::declval<scheduler_type&>(),
                                 std::declval<async_io::stream_socket_view>(),
                                 const_buffer(std::declval<Holder&>().data(),
                                              std::declval<Holder&>().size()),
                                 int{}));
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  tcp_send_operation(scheduler_type scheduler,
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

  tcp_send_operation(const tcp_send_operation&) = delete;
  tcp_send_operation& operator=(const tcp_send_operation&) = delete;
  tcp_send_operation(tcp_send_operation&&) = delete;
  tcp_send_operation& operator=(tcp_send_operation&&) = delete;

  void start() noexcept { bexec::start(*child_operation_); }

 private:
  scheduler_type scheduler_;
  async_io::stream_socket_view socket_;
  Holder holder_;
  int flags_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

template <class Scheduler, class Holder, bool Direct>
class tcp_send_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  tcp_send_sender(Scheduler scheduler, async_io::stream_socket_view socket,
                  Holder holder, int flags)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        flags_(flags) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return tcp_send_operation<Scheduler, Holder, Direct,
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

template <class Scheduler, bool Direct, class Receiver>
class tcp_accept_operation {
 public:
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using receiver_type = std::remove_cvref_t<Receiver>;

  static auto make_child_sender(scheduler_type& scheduler,
                                async_io::listening_socket_view socket,
                                int flags) {
    if constexpr (Direct) {
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

template <class Scheduler, bool Direct>
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
    return tcp_accept_operation<Scheduler, Direct,
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
