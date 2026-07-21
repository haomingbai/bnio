#ifndef BNIO_DETAIL_IO_CONTEXT_TIMER_WAIT_H_
#ifndef BNIO_DETAIL_IO_CONTEXT_CLASS_H_
#include <bnio/io_context.h>
#else
#define BNIO_DETAIL_IO_CONTEXT_TIMER_WAIT_H_

namespace bnio::detail {

template <class Receiver>
class timer_wait_operation : public timer_operation_base {
 public:
  timer_wait_operation(steady_timer& timer, Receiver receiver)
      : timer_operation_base(timer.context()),
        timer_(&timer.timer_),
        receiver_(std::move(receiver)) {}

  void start() noexcept {
    if (stop_requested(receiver_)) {
      this->timer_context_->queue_timer_completion(
          *this, timer_completion_kind::stopped);
      return;
    }

    this->timer_context_->start_timer_wait(*this, *timer_);
  }

  void execute() noexcept override {
    const timer_completion_kind completion = this->timer_completion();
    if (completion == timer_completion_kind::stopped) {
      bexec::set_stopped(std::move(receiver_));
    } else {
      bexec::set_value(std::move(receiver_));
    }
  }

 private:
  detail::timer_slot* timer_;
  std::remove_cvref_t<Receiver> receiver_;
};

class timer_wait_sender {
 public:
  using completion_signatures =
      bexec::completion_signatures<bexec::set_value_t(),
                                   bexec::set_error_t(std::error_code),
                                   bexec::set_stopped_t()>;

  explicit timer_wait_sender(steady_timer& timer) noexcept : timer_(&timer) {}

  template <class Receiver>
  auto connect(Receiver receiver) && {
    return timer_wait_operation<std::remove_cvref_t<Receiver> >(
        *timer_, std::move(receiver));
  }

  template <class Receiver>
  auto connect(Receiver receiver) const& {
    return timer_wait_operation<std::remove_cvref_t<Receiver> >(
        *timer_, std::move(receiver));
  }

 private:
  steady_timer* timer_;
};

}  // namespace bnio::detail

#endif  // BNIO_DETAIL_IO_CONTEXT_CLASS_H_
#endif  // BNIO_DETAIL_IO_CONTEXT_TIMER_WAIT_H_
