#pragma once
#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_WRITE_ALL_H_
#define BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_WRITE_ALL_H_

#include <bupp/linux/detail/io_context_native_io/common.h>
#include <bupp/linux/detail/io_context_native_io/file.h>
#include <bupp/linux/detail/io_context_native_io/socket.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/detail/manual_lifetime.hpp>
#include <bexec/receiver.hpp>
#include <bexec/repeat_until.hpp>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bupp {

/** @cond BUPP_DETAIL */
namespace detail {

class socket_write_all_state {
 public:
  socket_write_all_state(io_context& context,
                         async_io::stream_socket_view socket,
                         const_buffer buffer, int flags,
                         submit_mode mode) noexcept
      : context(&context),
        socket(socket),
        buffer(buffer),
        flags(flags),
        mode(mode) {}

  [[nodiscard]] std::size_t remaining() const noexcept {
    return buffer.size() - transferred;
  }

  [[nodiscard]] bool empty() const noexcept { return buffer.size() == 0; }

  [[nodiscard]] const_buffer current_buffer() const noexcept {
    const auto* data = static_cast<const char*>(buffer.data());
    return const_buffer(data + transferred, remaining());
  }

  [[nodiscard]] auto make_sender() noexcept {
    return native_io_sender<socket_write_model>(
        *context, socket_write_model(socket, current_buffer(), flags), mode);
  }

  void advance(std::size_t bytes) noexcept {
    transferred += bytes;
    if (transferred >= buffer.size()) {
      done = true;
    }
  }

  io_context* context;
  async_io::stream_socket_view socket;
  const_buffer buffer;
  int flags;
  submit_mode mode;
  std::size_t transferred = 0;
  bool done = false;
};

class descriptor_write_all_state {
 public:
  descriptor_write_all_state(io_context& context,
                             async_io::descriptor_view descriptor,
                             const_buffer buffer, std::uint64_t offset,
                             submit_mode mode) noexcept
      : context(&context),
        descriptor(descriptor),
        buffer(buffer),
        offset(offset),
        mode(mode) {}

  [[nodiscard]] std::size_t remaining() const noexcept {
    return buffer.size() - transferred;
  }

  [[nodiscard]] bool empty() const noexcept { return buffer.size() == 0; }

  [[nodiscard]] const_buffer current_buffer() const noexcept {
    const auto* data = static_cast<const char*>(buffer.data());
    return const_buffer(data + transferred, remaining());
  }

  [[nodiscard]] auto make_sender() noexcept {
    return native_io_sender<write_model>(
        *context,
        write_model(descriptor, current_buffer(), offset + transferred), mode);
  }

  void advance(std::size_t bytes) noexcept {
    transferred += bytes;
    if (transferred >= buffer.size()) {
      done = true;
    }
  }

  io_context* context;
  async_io::descriptor_view descriptor;
  const_buffer buffer;
  std::uint64_t offset;
  submit_mode mode;
  std::size_t transferred = 0;
  bool done = false;
};

template <class State, class Receiver>
class write_all_step_operation {
 public:
  using receiver_type = std::remove_cvref_t<Receiver>;

  class child_receiver {
   public:
    explicit child_receiver(write_all_step_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] decltype(auto) get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::size_t bytes) noexcept {
      operation_->handle_value(bytes);
    }

    void set_error(std::error_code error) noexcept {
      operation_->complete_error(error);
    }

    void set_stopped() noexcept { operation_->complete_stopped(); }

   private:
    write_all_step_operation* operation_;
  };

  using child_sender_type = decltype(std::declval<State&>().make_sender());
  using child_operation_type = decltype(bexec::connect(
      std::declval<child_sender_type>(), std::declval<child_receiver>()));

  write_all_step_operation(State* state, Receiver receiver)
      : state_(state), receiver_(std::move(receiver)) {}

  void start() noexcept {
    if (state_->remaining() == 0) {
      complete_value(0);
      return;
    }

    child_operation_.emplace_from([this] {
      return bexec::connect(state_->make_sender(), child_receiver(*this));
    });
    bexec::start(*child_operation_);
  }

 private:
  void handle_value(std::size_t bytes) noexcept {
    if (bytes == 0) {
      complete_error(std::make_error_code(std::errc::broken_pipe));
      return;
    }
    if (bytes > state_->remaining()) {
      complete_error(std::make_error_code(std::errc::protocol_error));
      return;
    }

    state_->advance(bytes);
    complete_value(bytes);
  }

  void complete_value(std::size_t bytes) noexcept {
    bexec::set_value(std::move(receiver_), bytes);
  }

  void complete_error(std::error_code error) noexcept {
    bexec::set_error(std::move(receiver_), error);
  }

  void complete_stopped() noexcept { bexec::set_stopped(std::move(receiver_)); }

  State* state_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

template <class State>
class write_all_step_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  explicit write_all_step_sender(State* state) noexcept : state_(state) {}

  template <class Receiver>
  auto connect(Receiver receiver) const {
    return write_all_step_operation<State, std::remove_cvref_t<Receiver>>(
        state_, std::move(receiver));
  }

 private:
  State* state_;
};

template <class State>
class write_all_step_factory {
 public:
  explicit write_all_step_factory(State* state) noexcept : state_(state) {}

  [[nodiscard]] auto operator()() const noexcept {
    return write_all_step_sender<State>(state_);
  }

 private:
  State* state_;
};

template <class State>
class write_all_done_predicate {
 public:
  explicit write_all_done_predicate(State* state) noexcept : state_(state) {}

  [[nodiscard]] bool operator()() const noexcept { return state_->done; }

 private:
  State* state_;
};

template <class State, class Receiver>
class write_all_operation {
 public:
  using receiver_type = std::remove_cvref_t<Receiver>;
  using factory_type = write_all_step_factory<State>;
  using predicate_type = write_all_done_predicate<State>;
  using repeat_sender_type = decltype(bexec::repeat_until(
      std::declval<factory_type>(), std::declval<predicate_type>()));

  class repeat_receiver {
   public:
    explicit repeat_receiver(write_all_operation& operation) noexcept
        : operation_(&operation) {}

    [[nodiscard]] decltype(auto) get_env() const noexcept {
      return bexec::get_env(operation_->receiver_);
    }

    void set_value(std::size_t) noexcept { operation_->complete_value(); }

    template <class Error>
    void set_error(Error&& error) noexcept {
      operation_->complete_error(std::forward<Error>(error));
    }

    void set_stopped() noexcept { operation_->complete_stopped(); }

   private:
    write_all_operation* operation_;
  };

  using repeat_operation_type = decltype(bexec::connect(
      std::declval<repeat_sender_type>(), std::declval<repeat_receiver>()));

  write_all_operation(State state, Receiver receiver)
      : state_(std::move(state)), receiver_(std::move(receiver)) {
    repeat_operation_.emplace_from([this] {
      return bexec::connect(
          bexec::repeat_until(factory_type(&state_), predicate_type(&state_)),
          repeat_receiver(*this));
    });
  }

  write_all_operation(const write_all_operation&) = delete;
  write_all_operation& operator=(const write_all_operation&) = delete;
  write_all_operation(write_all_operation&&) = delete;
  write_all_operation& operator=(write_all_operation&&) = delete;

  void start() noexcept {
    if (stop_requested(receiver_)) {
      complete_stopped();
      return;
    }
    if (state_.empty()) {
      complete_value();
      return;
    }

    bexec::start(*repeat_operation_);
  }

 private:
  void complete_value() noexcept {
    bexec::set_value(std::move(receiver_), state_.transferred);
  }

  void complete_error(std::error_code error) noexcept {
    bexec::set_error(std::move(receiver_), error);
  }

  template <class Error>
  void complete_error(Error&&) noexcept {
    bexec::set_error(std::move(receiver_),
                     std::make_error_code(std::errc::protocol_error));
  }

  void complete_stopped() noexcept { bexec::set_stopped(std::move(receiver_)); }

  State state_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<repeat_operation_type> repeat_operation_;
};

template <class State>
class write_all_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(std::size_t),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  explicit write_all_sender(State state) noexcept : state_(std::move(state)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return write_all_operation<State, std::remove_cvref_t<Receiver>>(
        std::move(state_), std::move(receiver));
  }

  template <class Receiver>
  auto connect(Receiver receiver) const& {
    return write_all_operation<State, std::remove_cvref_t<Receiver>>(
        state_, std::move(receiver));
  }

 private:
  State state_;
};

}  // namespace detail
/** @endcond */

}  // namespace bupp

#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_WRITE_ALL_H_
