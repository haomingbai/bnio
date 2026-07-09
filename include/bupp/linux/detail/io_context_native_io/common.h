#pragma once
#ifndef BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_COMMON_H_
#define BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_COMMON_H_

#include <bupp/base/linux/submission_queue_entry.h>
#include <bupp/linux/io_context.h>

#include <bexec/completion_signatures.hpp>
#include <bexec/query.hpp>
#include <bexec/receiver.hpp>
#include <concepts>
#include <system_error>
#include <type_traits>
#include <utility>

namespace bupp {

/** @cond BUPP_DETAIL */
namespace detail {

[[nodiscard]] inline std::error_code errno_result(int result) noexcept {
  return std::error_code(-result, std::generic_category());
}

template <class Receiver>
[[nodiscard]] bool stop_requested(const Receiver& receiver) noexcept {
  auto env = bexec::get_env(receiver);
  auto token = bexec::query(env, bexec::get_stop_token);
  return token.stop_requested();
}

template <class Model, class Receiver>
class native_io_operation : public io_context::operation_base {
 public:
  native_io_operation(io_context& context, Model model, submit_mode mode,
                      Receiver receiver)
      : context_(&context),
        model_(std::move(model)),
        mode_(mode),
        receiver_(std::move(receiver)) {}

  void prepare(bupp::base::submission_queue_entry& sqe) noexcept override {
    model_.prepare(sqe);
  }

  [[nodiscard]] int prepare_for_submit() noexcept override {
    completion_ = completion_kind::value;
    return context_->native_context().prepare(*this);
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

    completion_ = completion_kind::value;
    if (mode_ == submit_mode::direct) {
      context_->submit_direct(*this);
    } else {
      context_->enqueue_io(*this);
    }
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
  enum class completion_kind {
    value,
    error,
    stopped,
  };

  io_context* context_;
  Model model_;
  submit_mode mode_;
  std::remove_cvref_t<Receiver> receiver_;
  completion_kind completion_ = completion_kind::value;
  std::error_code error_;
};

template <class Model>
class native_io_sender {
 public:
  using completion_signatures = typename Model::completion_signatures;

  native_io_sender(io_context& context, Model model, submit_mode mode) noexcept
      : context_(&context), model_(std::move(model)), mode_(mode) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return native_io_operation<Model, std::remove_cvref_t<Receiver>>(
        *context_, std::move(model_), mode_, std::move(receiver));
  }

  template <class Receiver>
    requires std::copy_constructible<Model>
  auto connect(Receiver receiver) const& {
    return native_io_operation<Model, std::remove_cvref_t<Receiver>>(
        *context_, model_, mode_, std::move(receiver));
  }

 private:
  io_context* context_;
  Model model_;
  submit_mode mode_;
};

}  // namespace detail
/** @endcond */

}  // namespace bupp

#endif  // BUPP_LINUX_DETAIL_IO_CONTEXT_NATIVE_IO_COMMON_H_
