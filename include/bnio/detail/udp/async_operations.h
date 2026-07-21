/**
 * @file async_operations.h
 * @brief Internal UDP async operation implementations.
 */

#pragma once
#ifndef BNIO_DETAIL_UDP_ASYNC_OPERATIONS_H_
#define BNIO_DETAIL_UDP_ASYNC_OPERATIONS_H_

#include <bnio/async_io/socket_view.h>
#include <bnio/buffer.h>
#include <bnio/ip.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/detail/manual_lifetime.hpp>
#include <bexec/receiver.hpp>
#include <bexec/sender.hpp>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio::detail {

template <class Scheduler, class Holder, bool From, class Receiver>
class udp_receive_operation {
 public:
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using receiver_type = std::remove_cvref_t<Receiver>;

  class child_receiver {
   public:
    explicit child_receiver(udp_receive_operation& operation) noexcept
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
    udp_receive_operation* operation_;
  };

  static auto make_child_sender(scheduler_type& scheduler,
                                async_io::datagram_socket_view socket,
                                mutable_buffer buffer, ip::endpoint* endpoint,
                                int flags) {
    if constexpr (From) {
      return scheduler.async_receive_from(socket, buffer, *endpoint, flags);
    } else {
      return scheduler.async_receive(socket, buffer, flags);
    }
  }

  using child_sender_type = decltype(make_child_sender(
      std::declval<scheduler_type&>(),
      std::declval<async_io::datagram_socket_view>(),
      std::declval<mutable_buffer>(), static_cast<ip::endpoint*>(nullptr), 0));
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  udp_receive_operation(scheduler_type scheduler,
                        async_io::datagram_socket_view socket, Holder holder,
                        ip::endpoint* endpoint, int flags, Receiver receiver)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        endpoint_(endpoint),
        flags_(flags),
        receiver_(std::move(receiver)) {
    child_operation_.emplace_from([this] {
      return bexec::connect(
          make_child_sender(scheduler_, socket_, bnio::buffer(holder_.view()),
                            endpoint_, flags_),
          child_receiver(*this));
    });
  }

  udp_receive_operation(const udp_receive_operation&) = delete;
  udp_receive_operation& operator=(const udp_receive_operation&) = delete;
  udp_receive_operation(udp_receive_operation&&) = delete;
  udp_receive_operation& operator=(udp_receive_operation&&) = delete;

  void start() noexcept { bexec::start(*child_operation_); }

 private:
  scheduler_type scheduler_;
  async_io::datagram_socket_view socket_;
  Holder holder_;
  ip::endpoint* endpoint_;
  int flags_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

template <class Scheduler, class Holder, bool From>
class udp_receive_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  udp_receive_sender(Scheduler scheduler, async_io::datagram_socket_view socket,
                     Holder holder, ip::endpoint* endpoint, int flags)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        endpoint_(endpoint),
        flags_(flags) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return udp_receive_operation<Scheduler, Holder, From,
                                 std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), socket_, std::move(holder_), endpoint_, flags_,
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  async_io::datagram_socket_view socket_;
  Holder holder_;
  ip::endpoint* endpoint_;
  int flags_;
};

template <class Scheduler, class Holder, bool To, class Receiver>
class udp_send_operation {
 public:
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using receiver_type = std::remove_cvref_t<Receiver>;

  class child_receiver {
   public:
    explicit child_receiver(udp_send_operation& operation) noexcept
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
    udp_send_operation* operation_;
  };

  static auto make_child_sender(scheduler_type& scheduler,
                                async_io::datagram_socket_view socket,
                                const_buffer buffer,
                                const ip::endpoint& endpoint, int flags) {
    if constexpr (To) {
      return scheduler.async_send_to(socket, buffer, endpoint, flags);
    } else {
      return scheduler.async_send(socket, buffer, flags);
    }
  }

  using child_sender_type = decltype(make_child_sender(
      std::declval<scheduler_type&>(),
      std::declval<async_io::datagram_socket_view>(),
      std::declval<const_buffer>(), std::declval<const ip::endpoint&>(), 0));
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  udp_send_operation(scheduler_type scheduler,
                     async_io::datagram_socket_view socket, Holder holder,
                     ip::endpoint endpoint, int flags, Receiver receiver)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        endpoint_(endpoint),
        flags_(flags),
        receiver_(std::move(receiver)) {
    child_operation_.emplace_from([this] {
      return bexec::connect(
          make_child_sender(scheduler_, socket_,
                            const_buffer(holder_.data(), holder_.size()),
                            endpoint_, flags_),
          child_receiver(*this));
    });
  }

  udp_send_operation(const udp_send_operation&) = delete;
  udp_send_operation& operator=(const udp_send_operation&) = delete;
  udp_send_operation(udp_send_operation&&) = delete;
  udp_send_operation& operator=(udp_send_operation&&) = delete;

  void start() noexcept { bexec::start(*child_operation_); }

 private:
  scheduler_type scheduler_;
  async_io::datagram_socket_view socket_;
  Holder holder_;
  ip::endpoint endpoint_;
  int flags_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

template <class Scheduler, class Holder, bool To>
class udp_send_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  udp_send_sender(Scheduler scheduler, async_io::datagram_socket_view socket,
                  Holder holder, ip::endpoint endpoint, int flags)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        endpoint_(endpoint),
        flags_(flags) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return udp_send_operation<Scheduler, Holder, To,
                              std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), socket_, std::move(holder_), endpoint_, flags_,
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  async_io::datagram_socket_view socket_;
  Holder holder_;
  ip::endpoint endpoint_;
  int flags_;
};

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_UDP_ASYNC_OPERATIONS_H_
