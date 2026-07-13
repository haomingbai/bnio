#pragma once
#ifndef BUPP_ASYNC_IO_BSD_KQUEUE_OPERATIONS_FILE_H_
#define BUPP_ASYNC_IO_BSD_KQUEUE_OPERATIONS_FILE_H_

#include <bupp/async_io/bsd/detail/kqueue_receiver_operation.h>
#include <bupp/async_io/buffer_view.h>
#include <bupp/async_io/descriptor_view.h>

#include <utility>

namespace bupp::async_io::bsd_native {

/** Operation that performs one context-owned read after read readiness. */
template <class Receiver>
class kqueue_read_operation
    : public detail::kqueue_receiver_operation<Receiver> {
 public:
  kqueue_read_operation(kqueue_context& context, descriptor_view descriptor,
                        buffer_view buffer, Receiver receiver)
      : detail::kqueue_receiver_operation<Receiver>(context,
                                                    std::move(receiver)),
        descriptor_(descriptor),
        buffer_(buffer) {}

  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_read(descriptor_.native_handle());
  }

  [[nodiscard]] buffer_view get_data() noexcept override { return buffer_; }

  void start() noexcept { this->start_io(*this); }

 private:
  descriptor_view descriptor_;
  buffer_view buffer_;
};

/** Operation that performs one context-owned write after write readiness. */
template <class Receiver>
class kqueue_write_operation
    : public detail::kqueue_receiver_operation<Receiver> {
 public:
  kqueue_write_operation(kqueue_context& context, descriptor_view descriptor,
                         buffer_view buffer, Receiver receiver)
      : detail::kqueue_receiver_operation<Receiver>(context,
                                                    std::move(receiver)),
        descriptor_(descriptor),
        buffer_(buffer) {}

  void prepare(kqueue_helper& helper) noexcept {
    helper.prep_write(descriptor_.native_handle());
  }

  [[nodiscard]] buffer_view get_data() noexcept override { return buffer_; }

  void start() noexcept { this->start_io(*this); }

 private:
  descriptor_view descriptor_;
  buffer_view buffer_;
};

}  // namespace bupp::async_io::bsd_native

#endif  // BUPP_ASYNC_IO_BSD_KQUEUE_OPERATIONS_FILE_H_
