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

/**
 * Operation state for one datagram receive through a scheduler; owns the
 * buffer holder until completion and commits received bytes before completing
 * the parent. With From it also reports the source endpoint.
 */
template <class Scheduler, class Holder, bool From, class Receiver>
class udp_receive_operation {
 public:
  /** Stored scheduler type. */
  using scheduler_type = std::remove_cvref_t<Scheduler>;

  /** Parent receiver type. */
  using receiver_type = std::remove_cvref_t<Receiver>;

  /**
   * Receiver adapted onto the child sender: forwards the parent environment
   * and completes the parent receiver, committing received bytes on success.
   */
  class child_receiver {
   public:
    /** Environment type forwarded from the parent receiver. */
    // Named explicitly so get_env's return type is available while the
    // operation is incomplete; decltype(auto) would instantiate too early.
    using env_type = decltype(bexec::get_env(std::declval<receiver_type&>()));

    /** Binds the child receiver to its parent operation. */
    explicit child_receiver(udp_receive_operation& operation) noexcept
        : operation_(&operation) {}

    /** Returns the parent receiver's environment. */
    [[nodiscard]] env_type get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    /** Commits received bytes on success, then completes the parent. */
    void set_value(std::error_code ec, std::size_t size) noexcept {
      if (!ec) operation_->holder_.commit(size);
      bexec::set_value(std::move(operation_->receiver_), ec, size);
    }

    /** Completes the parent receiver as stopped. */
    void set_stopped() noexcept {
      bexec::set_stopped(std::move(operation_->receiver_));
    }

   private:
    udp_receive_operation* operation_;
  };

  /**
   * Creates the child sender selected by From.
   *
   * @param scheduler Scheduler providing the receive sender.
   * @param socket    Datagram socket to receive on.
   * @param buffer    Buffer that receives the datagram bytes.
   * @param endpoint  Source endpoint output; read only when From is true.
   * @param flags     Receive flags.
   */
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

  /** Child sender type produced by make_child_sender(). */
  using child_sender_type = decltype(make_child_sender(
      std::declval<scheduler_type&>(),
      std::declval<async_io::datagram_socket_view>(),
      std::declval<mutable_buffer>(), static_cast<ip::endpoint*>(nullptr), 0));

  /** Operation state type of the connected child sender and receiver. */
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  /**
   * Creates the operation and connects the child sender and receiver.
   *
   * @param scheduler Scheduler used for the child receive.
   * @param socket    Datagram socket to receive on.
   * @param holder    Buffer holder owning the receive storage.
   * @param endpoint  Source output; may be null only when From is false, and
   *                  must outlive the operation.
   * @param flags     Receive flags.
   * @param receiver  Parent receiver to complete.
   */
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

  /** Starts the child receive operation. */
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

/**
 * Sender for one datagram receive through a scheduler: endpoint-aware
 * (receive-from) when From, otherwise on the connected peer.
 */
template <class Scheduler, class Holder, bool From>
class udp_receive_sender {
 public:
  /** Completes with (std::error_code, std::size_t) or set_stopped(). */
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  /**
   * @p holder keeps the receive storage alive until completion. With From,
   * @p endpoint receives the source endpoint and must outlive the operation.
   */
  udp_receive_sender(Scheduler scheduler, async_io::datagram_socket_view socket,
                     Holder holder, ip::endpoint* endpoint, int flags)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        endpoint_(endpoint),
        flags_(flags) {}

  /** Connects to @p receiver and returns the operation state. */
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

/**
 * Operation state for one datagram send through a scheduler; owns the buffer
 * holder until completion. With To it sends to the stored endpoint, else to
 * the connected peer.
 */
template <class Scheduler, class Holder, bool To, class Receiver>
class udp_send_operation {
 public:
  /** Stored scheduler type. */
  using scheduler_type = std::remove_cvref_t<Scheduler>;

  /** Parent receiver type. */
  using receiver_type = std::remove_cvref_t<Receiver>;

  /**
   * Receiver adapted onto the child sender: forwards the parent environment
   * and the child completion to the parent receiver.
   */
  class child_receiver {
   public:
    /** Environment type forwarded from the parent receiver. */
    // Named explicitly so get_env's return type is available while the
    // operation is incomplete; decltype(auto) would instantiate too early.
    using env_type = decltype(bexec::get_env(std::declval<receiver_type&>()));

    /** Binds the child receiver to its parent operation. */
    explicit child_receiver(udp_send_operation& operation) noexcept
        : operation_(&operation) {}

    /** Returns the parent receiver's environment. */
    [[nodiscard]] env_type get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    /** Completes the parent receiver with @p ec and @p size. */
    void set_value(std::error_code ec, std::size_t size) noexcept {
      bexec::set_value(std::move(operation_->receiver_), ec, size);
    }

    /** Completes the parent receiver as stopped. */
    void set_stopped() noexcept {
      bexec::set_stopped(std::move(operation_->receiver_));
    }

   private:
    udp_send_operation* operation_;
  };

  /**
   * Creates the child sender selected by To.
   *
   * @param scheduler Scheduler providing the send sender.
   * @param socket    Datagram socket to send on.
   * @param buffer    Datagram bytes to send.
   * @param endpoint  Destination; read only when To is true.
   * @param flags     Send flags.
   */
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

  /** Child sender type produced by make_child_sender(). */
  using child_sender_type = decltype(make_child_sender(
      std::declval<scheduler_type&>(),
      std::declval<async_io::datagram_socket_view>(),
      std::declval<const_buffer>(), std::declval<const ip::endpoint&>(), 0));

  /** Operation state type of the connected child sender and receiver. */
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  /**
   * Creates the operation and connects the child sender and receiver.
   *
   * @param scheduler Scheduler used for the child send.
   * @param socket    Datagram socket to send on.
   * @param holder    Buffer holder owning the send storage.
   * @param endpoint  Destination; ignored when To is false.
   * @param flags     Send flags.
   * @param receiver  Parent receiver to complete.
   */
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

  /** Starts the child send operation. */
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

/**
 * Sender for one datagram send through a scheduler: to the stored endpoint
 * when To, otherwise on the connected peer.
 */
template <class Scheduler, class Holder, bool To>
class udp_send_sender {
 public:
  /** Completes with (std::error_code, std::size_t) or set_stopped(). */
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  /**
   * @p holder keeps the datagram bytes alive until completion. With To,
   * @p endpoint is the destination.
   */
  udp_send_sender(Scheduler scheduler, async_io::datagram_socket_view socket,
                  Holder holder, ip::endpoint endpoint, int flags)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        endpoint_(endpoint),
        flags_(flags) {}

  /** Connects to @p receiver and returns the operation state. */
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
