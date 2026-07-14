#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_COMMON_H_
#ifndef BUPP_LINUX_IO_CONTEXT_H_
#include <bupp/linux/io_context.h>
#else
#define BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_COMMON_H_

namespace bupp::detail {

[[nodiscard]] inline std::error_code errno_result(int result) noexcept {
  return std::error_code(-result, std::generic_category());
}

template <class Receiver>
[[nodiscard]] bool stop_requested(const Receiver& receiver) noexcept {
  auto env = bexec::get_env(receiver);
  auto token = bexec::query(env, bexec::get_stop_token);
  return token.stop_requested();
}

template <class Model>
concept has_immediate_io = requires(Model& model) {
  { model.try_immediate() } -> std::convertible_to<int>;
};

[[nodiscard]] inline bool should_wait_for_immediate_result(
    int result) noexcept {
  return result == -EAGAIN || result == -EWOULDBLOCK;
}

[[nodiscard]] inline int immediate_socket_result(ssize_t result) noexcept {
  if (result >= 0) {
    return static_cast<int>(result);
  }
  const int error = errno;
  if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK) {
    return -EAGAIN;
  }
  return -error;
}

[[nodiscard]] inline bool should_defer_nowait_error(int error) noexcept {
  return error == ENOSYS || error == EOPNOTSUPP || error == EINVAL ||
         error == ESPIPE;
}

[[nodiscard]] constexpr int nowait_read_flag() noexcept {
#ifdef RWF_NOWAIT
  return RWF_NOWAIT;
#else
  return 0x00000008;
#endif
}

[[nodiscard]] inline ssize_t pread_nowait(int descriptor, void* data,
                                          std::size_t size,
                                          std::uint64_t offset) noexcept {
#ifdef SYS_preadv2
  struct iovec view {
    data, size
  };
  const auto low = static_cast<unsigned long>(offset);
  unsigned long high = 0;
  if constexpr (sizeof(unsigned long) < sizeof(std::uint64_t)) {
    high = static_cast<unsigned long>(offset >> (sizeof(unsigned long) * 8U));
  }
  return ::syscall(SYS_preadv2, descriptor, &view, 1, low, high,
                   nowait_read_flag());
#else
  (void)descriptor;
  (void)data;
  (void)size;
  (void)offset;
  errno = ENOSYS;
  return -1;
#endif
}

[[nodiscard]] inline ssize_t pwrite_nowait(int descriptor, const void* data,
                                           std::size_t size,
                                           std::uint64_t offset) noexcept {
#ifdef SYS_pwritev2
  struct iovec view {
    const_cast<void*>(data), size
  };
  const auto low = static_cast<unsigned long>(offset);
  unsigned long high = 0;
  if constexpr (sizeof(unsigned long) < sizeof(std::uint64_t)) {
    high = static_cast<unsigned long>(offset >> (sizeof(unsigned long) * 8U));
  }
  return ::syscall(SYS_pwritev2, descriptor, &view, 1, low, high,
                   nowait_read_flag());
#else
  (void)descriptor;
  (void)data;
  (void)size;
  (void)offset;
  errno = ENOSYS;
  return -1;
#endif
}

template <class Model, class Receiver>
class native_io_operation : public io_context::operation_base {
 public:
  native_io_operation(io_context& context, Model model, Receiver receiver)
      : context_(&context),
        model_(std::move(model)),
        receiver_(std::move(receiver)) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    model_.prepare(sqe);
  }

  void complete_submit_error(int result) noexcept override {
    completion_ = completion_kind::error;
    error_ = errno_result(result);
  }

  void start() noexcept {
    if (stop_requested(receiver_)) {
      completion_ = completion_kind::stopped;
      context_->post(*this);
      return;
    }

    if (try_complete_immediate()) {
      return;
    }

    completion_ = completion_kind::value;
    context_->publish_io(*this);
  }

  void execute() noexcept override {
    switch (completion_) {
      case completion_kind::value:
        if (model_.is_error_result(this->result)) {
          bexec::set_error(std::move(receiver_),
                           model_.make_error(this->result));
        } else {
          model_.set_value(std::move(receiver_), this->result, this->flags);
        }
        break;
      case completion_kind::error:
        bexec::set_error(std::move(receiver_), error_);
        break;
      case completion_kind::stopped:
        bexec::set_stopped(std::move(receiver_));
        break;
    }
  }

 private:
  [[nodiscard]] bool try_complete_immediate() noexcept {
    if constexpr (has_immediate_io<Model>) {
      const int result = model_.try_immediate();
      if (should_wait_for_immediate_result(result)) {
        return false;
      }

      this->result = result;
      this->flags = 0;
      if (result < 0) {
        completion_ = completion_kind::error;
        error_ = errno_result(result);
      } else {
        completion_ = completion_kind::value;
      }
      context_->post(*this);
      return true;
    } else {
      return false;
    }
  }

  enum class completion_kind {
    value,
    error,
    stopped,
  };

  io_context* context_;
  Model model_;
  std::remove_cvref_t<Receiver> receiver_;
  completion_kind completion_ = completion_kind::value;
  std::error_code error_;
};

template <class Model>
class native_io_sender {
 public:
  using completion_signatures = typename Model::completion_signatures;

  native_io_sender(io_context& context, Model model) noexcept
      : context_(&context), model_(std::move(model)) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return native_io_operation<Model, std::remove_cvref_t<Receiver> >(
        *context_, std::move(model_), std::move(receiver));
  }

  template <class Receiver>
    requires std::copy_constructible<Model>
  auto connect(Receiver receiver) const& {
    return native_io_operation<Model, std::remove_cvref_t<Receiver> >(
        *context_, model_, std::move(receiver));
  }

 private:
  io_context* context_;
  Model model_;
};

}  // namespace bupp::detail

#endif  // BUPP_LINUX_IO_CONTEXT_H_
#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_COMMON_H_
