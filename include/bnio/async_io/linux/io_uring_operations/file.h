#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_FILE_H_
#define BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_FILE_H_

#include <bnio/async_io/buffer_view.h>
#include <bnio/async_io/descriptor_view.h>
#include <bnio/async_io/linux/detail/io_uring_receiver_operation.h>
#include <bnio/async_io/linux/io_uring_operations/helpers.h>
#include <bnio/async_io/linux/io_uring_operations/views.h>
#include <bnio/base/linux/submission_queue_entry.h>

#include <cstdint>
#include <utility>

namespace bnio::async_io::linux_native {

/**
 * Operation representing an io_uring read request.
 */
template <class Receiver>
class io_uring_read_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a read operation for a file descriptor buffer.
   */
  io_uring_read_operation(io_uring_context& context, descriptor_view descriptor,
                          const buffer_view& buffer, std::uint64_t offset,
                          Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        descriptor_(descriptor),
        buffer_(buffer),
        offset_(offset) {}

  /**
   * Prepares the read SQE.
   *
   * @see io_uring_prep_read
   */
  void prepare(bnio::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_read(descriptor_.native_handle(), buffer_.data,
                  detail::bounded_io_size(buffer_.size), offset_);
  }

  /**
   * Starts the read operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  descriptor_view descriptor_;
  buffer_view buffer_;
  std::uint64_t offset_;
};

/**
 * Operation representing an io_uring write request.
 */
template <class Receiver>
class io_uring_write_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a write operation for a file descriptor buffer.
   */
  io_uring_write_operation(io_uring_context& context,
                           descriptor_view descriptor,
                           const buffer_view& buffer, std::uint64_t offset,
                           Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        descriptor_(descriptor),
        buffer_(buffer),
        offset_(offset) {}

  /**
   * Prepares the write SQE.
   *
   * @see io_uring_prep_write
   */
  void prepare(bnio::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_write(descriptor_.native_handle(), buffer_.data,
                   detail::bounded_io_size(buffer_.size), offset_);
  }

  /**
   * Starts the write operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  descriptor_view descriptor_;
  buffer_view buffer_;
  std::uint64_t offset_;
};

/**
 * Operation representing an io_uring readv request.
 */
template <class Receiver>
class io_uring_readv_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a readv operation for a file descriptor and iovec array.
   */
  io_uring_readv_operation(io_uring_context& context,
                           descriptor_view descriptor,
                           buffer_sequence_view buffers, std::uint64_t offset,
                           Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        descriptor_(descriptor),
        buffers_(buffers),
        offset_(offset) {}

  /**
   * Prepares the readv SQE.
   *
   * @see io_uring_prep_readv
   */
  void prepare(bnio::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_readv(descriptor_.native_handle(), buffers_.native_data(),
                   buffers_.size(), offset_);
  }

  /**
   * Starts the readv operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  descriptor_view descriptor_;
  buffer_sequence_view buffers_;
  std::uint64_t offset_;
};

/**
 * Operation representing an io_uring writev request.
 */
template <class Receiver>
class io_uring_writev_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a writev operation for a file descriptor and iovec array.
   */
  io_uring_writev_operation(io_uring_context& context,
                            descriptor_view descriptor,
                            buffer_sequence_view buffers, std::uint64_t offset,
                            Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        descriptor_(descriptor),
        buffers_(buffers),
        offset_(offset) {}

  /**
   * Prepares the writev SQE.
   *
   * @see io_uring_prep_writev
   */
  void prepare(bnio::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_writev(descriptor_.native_handle(), buffers_.native_data(),
                    buffers_.size(), offset_);
  }

  /**
   * Starts the writev operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  descriptor_view descriptor_;
  buffer_sequence_view buffers_;
  std::uint64_t offset_;
};

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_FILE_H_
