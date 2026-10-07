/**
 * @file async_operations.h
 * @brief Internal local (AF_UNIX) async operation implementations.
 */

#pragma once
#ifndef BNIO_DETAIL_LOCAL_ASYNC_OPERATIONS_H_
#define BNIO_DETAIL_LOCAL_ASYNC_OPERATIONS_H_

#include <bnio/async_io/local/socket_view.h>
#include <bnio/buffer.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/detail/manual_lifetime.hpp>
#include <bexec/receiver.hpp>
#include <bexec/sender.hpp>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bnio {

namespace local {
class stream_socket;
}  // namespace local

/** @cond BNIO_DETAIL */
namespace detail {

template <class Scheduler, class Holder, bool Some, class Receiver>
class local_read_operation {
 public:
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using receiver_type = std::remove_cvref_t<Receiver>;

  static auto make_child_sender(scheduler_type& scheduler,
                                async_io::local::stream_socket_view socket,
                                async_io::buffer_view buffer, int flags) {
    if constexpr (Some) {
      return scheduler.async_read_some(socket, bnio::buffer(buffer), flags);
    } else {
      return scheduler.async_read(socket, bnio::buffer(buffer), flags);
    }
  }

  class child_receiver {
   public:
    // The env type depends only on the operation's Receiver template
    // parameter; naming it explicitly keeps get_env's return type available
    // while the operation class is still incomplete (child operation types
    // are computed inside its own definition). A deduced decltype(auto)
    // return would force the body — and its operation_->receiver_ access —
    // to be instantiated too early.
    using env_type = decltype(bexec::get_env(std::declval<receiver_type&>()));

    explicit child_receiver(local_read_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] env_type get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::error_code ec, std::size_t size) noexcept {
      if (!ec) operation_->holder_.commit(size);
      bexec::set_value(std::move(operation_->receiver_), ec, size);
    }

    void set_stopped() noexcept {
      bexec::set_stopped(std::move(operation_->receiver_));
    }

   private:
    local_read_operation* operation_;
  };

  using child_sender_type = decltype(make_child_sender(
      std::declval<scheduler_type&>(),
      std::declval<async_io::local::stream_socket_view>(),
      std::declval<Holder&>().view(), int{}));
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  local_read_operation(scheduler_type scheduler,
                       async_io::local::stream_socket_view socket,
                       Holder holder, int flags, Receiver receiver)
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

  local_read_operation(const local_read_operation&) = delete;
  local_read_operation& operator=(const local_read_operation&) = delete;
  local_read_operation(local_read_operation&&) = delete;
  local_read_operation& operator=(local_read_operation&&) = delete;

  void start() noexcept { bexec::start(*child_operation_); }

 private:
  scheduler_type scheduler_;
  async_io::local::stream_socket_view socket_;
  Holder holder_;
  int flags_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

template <class Scheduler, class Holder, bool Some>
class local_read_sender {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  local_read_sender(Scheduler scheduler,
                    async_io::local::stream_socket_view socket, Holder holder,
                    int flags)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        flags_(flags) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return local_read_operation<Scheduler, Holder, Some,
                                std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), socket_, std::move(holder_), flags_,
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  async_io::local::stream_socket_view socket_;
  Holder holder_;
  int flags_;
};

template <class Scheduler, class Holder, bool Some, class Receiver>
class local_write_operation {
 public:
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using receiver_type = std::remove_cvref_t<Receiver>;

  static auto make_child_sender(scheduler_type& scheduler,
                                async_io::local::stream_socket_view socket,
                                const_buffer buffer, int flags) {
    if constexpr (Some) {
      return scheduler.async_write_some(socket, buffer, flags);
    } else {
      return scheduler.async_write(socket, buffer, flags);
    }
  }

  class child_receiver {
   public:
    // The env type depends only on the operation's Receiver template
    // parameter; naming it explicitly keeps get_env's return type available
    // while the operation class is still incomplete (child operation types
    // are computed inside its own definition). A deduced decltype(auto)
    // return would force the body — and its operation_->receiver_ access —
    // to be instantiated too early.
    using env_type = decltype(bexec::get_env(std::declval<receiver_type&>()));

    explicit child_receiver(local_write_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] env_type get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::error_code ec, std::size_t size) noexcept {
      bexec::set_value(std::move(operation_->receiver_), ec, size);
    }

    void set_stopped() noexcept {
      bexec::set_stopped(std::move(operation_->receiver_));
    }

   private:
    local_write_operation* operation_;
  };

  using child_sender_type = decltype(make_child_sender(
      std::declval<scheduler_type&>(),
      std::declval<async_io::local::stream_socket_view>(),
      const_buffer(std::declval<Holder&>().data(),
                   std::declval<Holder&>().size()),
      int{}));
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  local_write_operation(scheduler_type scheduler,
                        async_io::local::stream_socket_view socket,
                        Holder holder, int flags, Receiver receiver)
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

  local_write_operation(const local_write_operation&) = delete;
  local_write_operation& operator=(const local_write_operation&) = delete;
  local_write_operation(local_write_operation&&) = delete;
  local_write_operation& operator=(local_write_operation&&) = delete;

  void start() noexcept { bexec::start(*child_operation_); }

 private:
  scheduler_type scheduler_;
  async_io::local::stream_socket_view socket_;
  Holder holder_;
  int flags_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

template <class Scheduler, class Holder, bool Some>
class local_write_sender {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  local_write_sender(Scheduler scheduler,
                     async_io::local::stream_socket_view socket, Holder holder,
                     int flags)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        flags_(flags) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return local_write_operation<Scheduler, Holder, Some,
                                 std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), socket_, std::move(holder_), flags_,
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  async_io::local::stream_socket_view socket_;
  Holder holder_;
  int flags_;
};

template <class Scheduler, class Receiver>
class local_accept_operation {
 public:
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using receiver_type = std::remove_cvref_t<Receiver>;

  static auto make_child_sender(scheduler_type& scheduler,
                                async_io::local::stream_socket_view socket,
                                int flags) {
    return scheduler.async_accept(socket, flags);
  }

  class child_receiver {
   public:
    // The env type depends only on the operation's Receiver template
    // parameter; naming it explicitly keeps get_env's return type available
    // while the operation class is still incomplete (child operation types
    // are computed inside its own definition). A deduced decltype(auto)
    // return would force the body — and its operation_->receiver_ access —
    // to be instantiated too early.
    using env_type = decltype(bexec::get_env(std::declval<receiver_type&>()));

    explicit child_receiver(local_accept_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] env_type get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::error_code ec, int fd) noexcept;

    void set_stopped() noexcept {
      bexec::set_stopped(std::move(operation_->receiver_));
    }

   private:
    local_accept_operation* operation_;
  };

  using child_sender_type = decltype(make_child_sender(
      std::declval<scheduler_type&>(),
      std::declval<async_io::local::stream_socket_view>(), int{}));
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  local_accept_operation(scheduler_type scheduler,
                         async_io::local::stream_socket_view socket, int flags,
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

  local_accept_operation(const local_accept_operation&) = delete;
  local_accept_operation& operator=(const local_accept_operation&) = delete;
  local_accept_operation(local_accept_operation&&) = delete;
  local_accept_operation& operator=(local_accept_operation&&) = delete;

  void start() noexcept { bexec::start(*child_operation_); }

 private:
  scheduler_type scheduler_;
  async_io::local::stream_socket_view socket_;
  int flags_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

template <class Scheduler>
class local_accept_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::error_code,
                                                      local::stream_socket),
                                   bexec::set_stopped_t()>;

  local_accept_sender(Scheduler scheduler,
                      async_io::local::stream_socket_view socket, int flags)
      : scheduler_(std::move(scheduler)), socket_(socket), flags_(flags) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return local_accept_operation<Scheduler, std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), socket_, flags_, std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  async_io::local::stream_socket_view socket_;
  int flags_;
};

template <class Scheduler, class Holder, bool From, class Receiver>
class local_receive_operation {
 public:
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using receiver_type = std::remove_cvref_t<Receiver>;

  static auto make_child_sender(scheduler_type& scheduler,
                                async_io::local::datagram_socket_view socket,
                                mutable_buffer buffer,
                                async_io::local::endpoint* endpoint,
                                int flags) {
    if constexpr (From) {
      return scheduler.async_receive_from(socket, buffer, *endpoint, flags);
    } else {
      return scheduler.async_receive(socket, buffer, flags);
    }
  }

  class child_receiver {
   public:
    // The env type depends only on the operation's Receiver template
    // parameter; naming it explicitly keeps get_env's return type available
    // while the operation class is still incomplete (child operation types
    // are computed inside its own definition). A deduced decltype(auto)
    // return would force the body — and its operation_->receiver_ access —
    // to be instantiated too early.
    using env_type = decltype(bexec::get_env(std::declval<receiver_type&>()));

    explicit child_receiver(local_receive_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] env_type get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::error_code ec, std::size_t size) noexcept {
      if (!ec) operation_->holder_.commit(size);
      bexec::set_value(std::move(operation_->receiver_), ec, size);
    }

    void set_stopped() noexcept {
      bexec::set_stopped(std::move(operation_->receiver_));
    }

   private:
    local_receive_operation* operation_;
  };

  using child_sender_type = decltype(make_child_sender(
      std::declval<scheduler_type&>(),
      std::declval<async_io::local::datagram_socket_view>(),
      std::declval<mutable_buffer>(),
      static_cast<async_io::local::endpoint*>(nullptr), 0));
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  local_receive_operation(scheduler_type scheduler,
                          async_io::local::datagram_socket_view socket,
                          Holder holder, async_io::local::endpoint* endpoint,
                          int flags, Receiver receiver)
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

  local_receive_operation(const local_receive_operation&) = delete;
  local_receive_operation& operator=(const local_receive_operation&) = delete;
  local_receive_operation(local_receive_operation&&) = delete;
  local_receive_operation& operator=(local_receive_operation&&) = delete;

  void start() noexcept { bexec::start(*child_operation_); }

 private:
  scheduler_type scheduler_;
  async_io::local::datagram_socket_view socket_;
  Holder holder_;
  async_io::local::endpoint* endpoint_;
  int flags_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

template <class Scheduler, class Holder, bool From>
class local_receive_sender {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  local_receive_sender(Scheduler scheduler,
                       async_io::local::datagram_socket_view socket,
                       Holder holder, async_io::local::endpoint* endpoint,
                       int flags)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        endpoint_(endpoint),
        flags_(flags) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return local_receive_operation<Scheduler, Holder, From,
                                   std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), socket_, std::move(holder_), endpoint_, flags_,
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  async_io::local::datagram_socket_view socket_;
  Holder holder_;
  async_io::local::endpoint* endpoint_;
  int flags_;
};

template <class Scheduler, class Holder, bool To, class Receiver>
class local_send_operation {
 public:
  using scheduler_type = std::remove_cvref_t<Scheduler>;
  using receiver_type = std::remove_cvref_t<Receiver>;

  static auto make_child_sender(scheduler_type& scheduler,
                                async_io::local::datagram_socket_view socket,
                                const_buffer buffer,
                                const async_io::local::endpoint& endpoint,
                                int flags) {
    if constexpr (To) {
      return scheduler.async_send_to(socket, buffer, endpoint, flags);
    } else {
      return scheduler.async_send(socket, buffer, flags);
    }
  }

  class child_receiver {
   public:
    // The env type depends only on the operation's Receiver template
    // parameter; naming it explicitly keeps get_env's return type available
    // while the operation class is still incomplete (child operation types
    // are computed inside its own definition). A deduced decltype(auto)
    // return would force the body — and its operation_->receiver_ access —
    // to be instantiated too early.
    using env_type = decltype(bexec::get_env(std::declval<receiver_type&>()));

    explicit child_receiver(local_send_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] env_type get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::error_code ec, std::size_t size) noexcept {
      bexec::set_value(std::move(operation_->receiver_), ec, size);
    }

    void set_stopped() noexcept {
      bexec::set_stopped(std::move(operation_->receiver_));
    }

   private:
    local_send_operation* operation_;
  };

  using child_sender_type = decltype(make_child_sender(
      std::declval<scheduler_type&>(),
      std::declval<async_io::local::datagram_socket_view>(),
      std::declval<const_buffer>(),
      std::declval<const async_io::local::endpoint&>(), 0));
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  local_send_operation(scheduler_type scheduler,
                       async_io::local::datagram_socket_view socket,
                       Holder holder, async_io::local::endpoint endpoint,
                       int flags, Receiver receiver)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        endpoint_(std::move(endpoint)),
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

  local_send_operation(const local_send_operation&) = delete;
  local_send_operation& operator=(const local_send_operation&) = delete;
  local_send_operation(local_send_operation&&) = delete;
  local_send_operation& operator=(local_send_operation&&) = delete;

  void start() noexcept { bexec::start(*child_operation_); }

 private:
  scheduler_type scheduler_;
  async_io::local::datagram_socket_view socket_;
  Holder holder_;
  async_io::local::endpoint endpoint_;
  int flags_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

template <class Scheduler, class Holder, bool To>
class local_send_sender {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  local_send_sender(Scheduler scheduler,
                    async_io::local::datagram_socket_view socket, Holder holder,
                    async_io::local::endpoint endpoint, int flags)
      : scheduler_(std::move(scheduler)),
        socket_(socket),
        holder_(std::move(holder)),
        endpoint_(std::move(endpoint)),
        flags_(flags) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return local_send_operation<Scheduler, Holder, To,
                                std::remove_cvref_t<Receiver>>(
        std::move(scheduler_), socket_, std::move(holder_), endpoint_, flags_,
        std::move(receiver));
  }

 private:
  Scheduler scheduler_;
  async_io::local::datagram_socket_view socket_;
  Holder holder_;
  async_io::local::endpoint endpoint_;
  int flags_;
};

}  // namespace detail
/** @endcond */

}  // namespace bnio

#endif  // BNIO_DETAIL_LOCAL_ASYNC_OPERATIONS_H_
