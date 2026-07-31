/**
 * @file write_all.h
 * @brief write-all composite async operation.
 */

#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_WRITE_ALL_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_POSIX_IO_CONTEXT_WRITE_ALL_H_

#include <bexec/detail/manual_lifetime.hpp>
#include <bexec/repeat_until.hpp>

namespace bnio::detail {

class socket_write_all_state {
 public:
  socket_write_all_state(io_context& context,
                         async_io::stream_socket_view socket,
                         const_buffer buffer, int flags) noexcept
      : context(&context), socket(socket), buffer(buffer), flags(flags) {}

  [[nodiscard]] std::size_t remaining() const noexcept {
    return buffer.size() - transferred;
  }

  [[nodiscard]] bool empty() const noexcept { return buffer.size() == 0; }

  [[nodiscard]] const_buffer current_buffer() const noexcept {
    const auto* data = static_cast<const char*>(buffer.data());
    return const_buffer(data + transferred, remaining());
  }

  [[nodiscard]] auto make_sender() noexcept {
    return native_io_sender(
        *context, make_stream_write_request(socket, current_buffer(), flags));
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
  std::size_t transferred = 0;
  bool done = false;
};

class descriptor_write_all_state {
 public:
  descriptor_write_all_state(io_context& context,
                             async_io::descriptor_view descriptor,
                             const_buffer buffer, std::uint64_t offset) noexcept
      : context(&context),
        descriptor(descriptor),
        buffer(buffer),
        offset(offset) {}

  [[nodiscard]] std::size_t remaining() const noexcept {
    return buffer.size() - transferred;
  }

  [[nodiscard]] bool empty() const noexcept { return buffer.size() == 0; }

  [[nodiscard]] const_buffer current_buffer() const noexcept {
    const auto* data = static_cast<const char*>(buffer.data());
    return const_buffer(data + transferred, remaining());
  }

  [[nodiscard]] auto make_sender() noexcept {
    return native_io_sender(
        *context, make_file_write_request(descriptor, current_buffer(),
                                          offset + transferred));
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

    void set_value(std::error_code ec, std::size_t bytes) noexcept {
      operation_->handle_value(ec, bytes);
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
      complete_value(std::error_code{}, state_->transferred);
      return;
    }

    child_operation_.emplace_from([this] {
      return bexec::connect(state_->make_sender(), child_receiver(*this));
    });
    bexec::start(*child_operation_);
  }

 private:
  void handle_value(std::error_code ec, std::size_t bytes) noexcept {
    if (ec) {
      // 失败/取消：终结。必须设 done=true 让 repeat_until 的 predicate
      // 退出循环，否则会无限重试同一个失败操作。
      state_->done = true;
      complete_value(ec, state_->transferred);
      return;
    }
    if (bytes == 0) {
      // EOF（对端关闭）：终结。
      state_->done = true;
      complete_value(std::make_error_code(std::errc::broken_pipe),
                     state_->transferred);
      return;
    }
    if (bytes > state_->remaining()) {
      // 不变量违规：终结。
      state_->done = true;
      complete_value(std::make_error_code(std::errc::protocol_error),
                     state_->transferred);
      return;
    }

    state_->advance(bytes);
    complete_value(std::error_code{}, state_->transferred);
  }

  void complete_value(std::error_code ec, std::size_t bytes_written) noexcept {
    bexec::set_value(std::move(receiver_), ec, bytes_written);
  }

  void complete_stopped() noexcept { bexec::set_stopped(std::move(receiver_)); }

  State* state_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<child_operation_type> child_operation_;
};

template <class State>
class write_all_step_sender {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  explicit write_all_step_sender(State* state) noexcept : state_(state) {}

  template <class Receiver>
  auto connect(Receiver receiver) const {
    return write_all_step_operation<State, std::remove_cvref_t<Receiver> >(
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

    void set_value(std::error_code ec, std::size_t bytes_written) noexcept {
      operation_->complete_value(ec, bytes_written);
    }

    template <class Error>
    void set_error(Error&& error) noexcept {
      // bexec::repeat_until 内部异常路径：透传给 complete_error（走
      // set_value(ec, bytes)）
      operation_->complete_error(std::forward<Error>(error));
    }

    void set_stopped() noexcept {
      // 区分 stop-token 取消 vs io_context::stop()
      // repeat_until::drain() 在每轮开始前检查 stop_token，若为 true 发
      // set_stopped — 这是 stop-token 取消 step::child_receiver::set_stopped()
      // 来自 transport 层的 io_context::stop() 中断 — 这是 set_stopped
      // 应保留的场景
      if (detail::stop_requested(operation_->receiver_)) {
        operation_->complete_canceled();
      } else {
        operation_->complete_stopped();
      }
    }

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
      // stop token 在启动前已请求：取消，带上已写字节数（=0）
      complete_value(std::make_error_code(std::errc::operation_canceled),
                     state_.transferred);
      return;
    }
    if (state_.empty()) {
      // 空缓冲：成功，0 字节
      complete_value(std::error_code{}, state_.transferred);
      return;
    }

    bexec::start(*repeat_operation_);
  }

 private:
  void complete_value(std::error_code ec, std::size_t bytes_written) noexcept {
    bexec::set_value(std::move(receiver_), ec, bytes_written);
  }

  void complete_canceled() noexcept {
    // stop-token 取消：上报 ec=operation_canceled 与已写累计
    bexec::set_value(std::move(receiver_),
                     std::make_error_code(std::errc::operation_canceled),
                     state_.transferred);
  }

  void complete_error(std::error_code error) noexcept {
    // 不可恢复内部异常：通过 set_value(ec, bytes_written) 透传给 receiver
    bexec::set_value(std::move(receiver_), error, state_.transferred);
  }

  template <class Error>
  void complete_error(Error&&) noexcept {
    // 不可恢复内部异常（未知错误类型）
    bexec::set_value(std::move(receiver_),
                     std::make_error_code(std::errc::protocol_error),
                     state_.transferred);
  }

  void complete_stopped() noexcept { bexec::set_stopped(std::move(receiver_)); }

  State state_;
  receiver_type receiver_;
  bexec::detail::manual_lifetime<repeat_operation_type> repeat_operation_;
};

template <class State>
class write_all_sender {
 public:
  using completion_signatures = bexec::completion_signatures<
      bexec::set_value_t(std::error_code, std::size_t), bexec::set_stopped_t()>;

  explicit write_all_sender(State state) noexcept : state_(std::move(state)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return write_all_operation<State, std::remove_cvref_t<Receiver> >(
        std::move(state_), std::move(receiver));
  }

  template <class Receiver>
  auto connect(Receiver receiver) const& {
    return write_all_operation<State, std::remove_cvref_t<Receiver> >(
        state_, std::move(receiver));
  }

 private:
  State state_;
};

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_WRITE_ALL_H_
