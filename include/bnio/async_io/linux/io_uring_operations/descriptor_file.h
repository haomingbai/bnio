/**
 * @file descriptor_file.h
 * @brief io_uring streaming file read/write operations.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_DESCRIPTOR_FILE_H_
#define BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_DESCRIPTOR_FILE_H_

#include <bnio/async_io/buffer_view.h>
#include <bnio/async_io/descriptor_view.h>
#include <bnio/async_io/linux/detail/io_uring_receiver_operation.h>
#include <bnio/async_io/linux/io_uring_operations/helpers.h>
#include <bnio/async_io/linux/io_uring_operations/views.h>
#include <bnio/base/linux/submission_queue_entry.h>

#include <cstdint>
#include <utility>

namespace bnio::async_io::linux_native {

namespace detail {

/** SQE offset -1: use and advance the kernel file position (streaming). */
inline constexpr std::uint64_t stream_file_offset =
    static_cast<std::uint64_t>(-1);

}  // namespace detail

/**
 * Operation representing a streaming io_uring read request: the SQE carries
 * offset -1, so the kernel uses and advances the file position.
 */
template <class Receiver>
class io_uring_read_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a streaming read operation for a file descriptor buffer.
   */
  io_uring_read_operation(io_uring_context& context, descriptor_view descriptor,
                          const buffer_view& buffer, Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        descriptor_(descriptor),
        buffer_(buffer) {
    this->enable_eagain_rearm();
  }

  /**
   * Prepares the read SQE.
   *
   * @see io_uring_prep_read
   */
  void prepare(bnio::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_read(descriptor_.native_handle(), buffer_.data,
                  detail::bounded_io_size(buffer_.size),
                  detail::stream_file_offset);
  }

  /**
   * Starts the read operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  descriptor_view descriptor_;
  buffer_view buffer_;
};

/**
 * Operation representing a streaming io_uring write request: the SQE
 * carries offset -1, so the kernel uses and advances the file position.
 */
template <class Receiver>
class io_uring_write_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a streaming write operation for a file descriptor buffer.
   */
  io_uring_write_operation(io_uring_context& context,
                           descriptor_view descriptor,
                           const buffer_view& buffer, Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        descriptor_(descriptor),
        buffer_(buffer) {
    this->enable_eagain_rearm();
  }

  /**
   * Prepares the write SQE.
   *
   * @see io_uring_prep_write
   */
  void prepare(bnio::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_write(descriptor_.native_handle(), buffer_.data,
                   detail::bounded_io_size(buffer_.size),
                   detail::stream_file_offset);
  }

  /**
   * Starts the write operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  descriptor_view descriptor_;
  buffer_view buffer_;
};

/**
 * Operation representing a streaming io_uring readv request: the SQE
 * carries offset -1, so the kernel uses and advances the file position.
 */
template <class Receiver>
class io_uring_readv_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a streaming readv operation for a descriptor and iovec array.
   */
  io_uring_readv_operation(io_uring_context& context,
                           descriptor_view descriptor,
                           buffer_sequence_view buffers, Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        descriptor_(descriptor),
        buffers_(buffers) {
    this->enable_eagain_rearm();
  }

  /**
   * Prepares the readv SQE.
   *
   * @see io_uring_prep_readv
   */
  void prepare(bnio::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_readv(descriptor_.native_handle(), buffers_.native_data(),
                   buffers_.size(), detail::stream_file_offset);
  }

  /**
   * Starts the readv operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  descriptor_view descriptor_;
  buffer_sequence_view buffers_;
};

/**
 * Operation representing a streaming io_uring writev request: the SQE
 * carries offset -1, so the kernel uses and advances the file position.
 */
template <class Receiver>
class io_uring_writev_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a streaming writev operation for a descriptor and iovec array.
   */
  io_uring_writev_operation(io_uring_context& context,
                            descriptor_view descriptor,
                            buffer_sequence_view buffers, Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        descriptor_(descriptor),
        buffers_(buffers) {
    this->enable_eagain_rearm();
  }

  /**
   * Prepares the writev SQE.
   *
   * @see io_uring_prep_writev
   */
  void prepare(bnio::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_writev(descriptor_.native_handle(), buffers_.native_data(),
                    buffers_.size(), detail::stream_file_offset);
  }

  /**
   * Starts the writev operation.
   */
  void start() noexcept { this->start_io(*this); }

 private:
  descriptor_view descriptor_;
  buffer_sequence_view buffers_;
};

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_DESCRIPTOR_FILE_H_
