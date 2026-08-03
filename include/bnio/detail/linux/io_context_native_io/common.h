/**
 * @file common.h
 * @brief Common Linux native I/O operation support.
 */

#ifndef BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_COMMON_H_
#ifndef BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_COMMON_H_

#include <bnio/async_io/dns/resolve.h>

#include <algorithm>
#include <cerrno>
#include <system_error>

namespace bnio::detail {

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
  struct iovec view{data, size};
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
  struct iovec view{const_cast<void*>(data), size};
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

  void prepare(bnio::base::submission_queue_entry& sqe) noexcept override {
    model_.prepare(sqe);
  }

  void complete_submit_error(int result) noexcept override {
    // SQE prep failure: route through set_value(ec, ...).
    completion_ = completion_kind::value_with_ec;
    error_ = errno_result(result);
    this->result = result;
    this->flags = 0;
  }

  void complete_submit_stopped() noexcept override {
    // io_context::stop() -> abort_inflight_io: the only set_stopped entry.
    completion_ = completion_kind::stopped;
  }

  void start() noexcept {
    if (stop_requested(receiver_)) {
      // stop-token cancel: route through set_value(operation_canceled, ...).
      completion_ = completion_kind::value_with_ec;
      error_ = std::make_error_code(std::errc::operation_canceled);
      this->result = 0;
      this->flags = 0;
      context_->publish_cpu(*this);
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
        // Native call succeeded, or a CQE completed with a negative result
        // (-errno). Re-derive ec from result so the errno surfaces via
        // set_value(ec, ...) rather than being lost (§9.2 guard).
        if (this->result < 0) {
          model_.set_value(std::move(receiver_), errno_result(this->result),
                           this->result, this->flags);
        } else {
          model_.set_value(std::move(receiver_), std::error_code{},
                           this->result, this->flags);
        }
        break;
      case completion_kind::value_with_ec:
        // errno / SQE prep failure / stop-token cancel: error_ carries ec.
        model_.set_value(std::move(receiver_), error_, this->result,
                         this->flags);
        break;
      case completion_kind::stopped:
        // Sole source: io_context::stop() -> abort_inflight_io
        //              -> complete_submit_stopped().
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
        // errno routes through the value channel via set_value(ec, ...).
        completion_ = completion_kind::value_with_ec;
        error_ = errno_result(result);
      } else {
        completion_ = completion_kind::value;
      }
      context_->publish_cpu(*this);
      return true;
    } else {
      return false;
    }
  }

  enum class completion_kind {
    value,          // success, ec={}
    value_with_ec,  // errno / cancel / submit failure -> set_value(ec, ...)
    stopped,        // only io_context::stop()
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

template <class Receiver>
class resolve_operation
    : public async_io::linux_native::io_uring_operation_base {
 public:
  resolve_operation(io_context& context, async_io::dns_query query,
                    async_io::dns_result_view result, Receiver receiver)
      : context_(&context),
        query_(std::move(query)),
        result_(result),
        receiver_(std::move(receiver)) {}

  void start() noexcept {
    canceled_ = stop_requested(receiver_);
    context_->publish_cpu(*this);
  }

  void execute() noexcept override {
    if (canceled_) {
      // stop-token cancel: set_value(operation_canceled, 0).
      bexec::set_value(std::move(receiver_),
                       std::make_error_code(std::errc::operation_canceled),
                       std::size_t{0});
      return;
    }

    std::size_t count = 0;
    const std::error_code ec = async_io::resolve_dns(query_, result_, count);
    // Success ec={}; failure ec carries the resolver error. Both exit through
    // set_value(ec, count). resolve runs on the CPU queue, so io_context::stop
    // never reaches it and set_stopped is not produced here.
    bexec::set_value(std::move(receiver_), ec, count);
  }

 private:
  io_context* context_;
  async_io::dns_query query_;
  async_io::dns_result_view result_;
  std::remove_cvref_t<Receiver> receiver_;
  bool canceled_ = false;
};

class resolve_sender {
 public:
  using completion_signatures = bexec::completion_signatures<bexec::set_value_t(
      std::error_code, std::size_t)>;

  resolve_sender(io_context& context, async_io::dns_query query,
                 async_io::dns_result_view result)
      : context_(&context), query_(std::move(query)), result_(result) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return resolve_operation<std::remove_cvref_t<Receiver> >(
        *context_, std::move(query_), result_, std::move(receiver));
  }

  template <class Receiver>
  auto connect(Receiver receiver) const& {
    return resolve_operation<std::remove_cvref_t<Receiver> >(
        *context_, query_, result_, std::move(receiver));
  }

 private:
  io_context* context_;
  async_io::dns_query query_;
  async_io::dns_result_view result_;
};

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_POSIX_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_LINUX_IO_CONTEXT_NATIVE_IO_COMMON_H_
