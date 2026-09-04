/**
 * @file file.h
 * @brief io_uring streaming and positioned file read/write operations.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_FILE_H_
#define BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_FILE_H_

#include <bnio/async_io/buffer_view.h>
#include <bnio/async_io/descriptor_view.h>
#include <bnio/async_io/linux/detail/io_uring_receiver_operation.h>
#include <bnio/async_io/linux/io_uring_operations/helpers.h>
#include <bnio/async_io/linux/io_uring_operations/views.h>
#include <bnio/async_io/random_access_file.h>
#include <bnio/base/linux/submission_queue_entry.h>

#include <cerrno>
#include <cstdint>
#include <limits>
#include <utility>

namespace bnio::async_io::linux_native {

namespace detail {

/** SQE offset -1: use and advance the kernel file position (streaming). */
inline constexpr std::uint64_t stream_file_offset =
    static_cast<std::uint64_t>(-1);

/** Positioned offsets must fit the kernel's signed loff_t. */
[[nodiscard]] inline bool valid_random_access_offset(
    std::uint64_t offset) noexcept {
  return offset <=
         static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

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

/**
 * Operation representing a positioned io_uring read request on a random
 * access file: the SQE carries the explicit offset, so the kernel file
 * position is neither observed nor advanced. Offsets beyond the kernel's
 * signed loff_t are rejected with EOVERFLOW before entering the kernel.
 */
template <class Receiver>
class io_uring_random_access_read_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a positioned read operation for a random access file buffer.
   */
  io_uring_random_access_read_operation(io_uring_context& context,
                                        random_access_file file,
                                        const buffer_view& buffer,
                                        std::uint64_t offset,
                                        Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        file_(file),
        buffer_(buffer),
        offset_(offset) {
    this->enable_eagain_rearm();
  }

  /**
   * Prepares the read SQE.
   *
   * @see io_uring_prep_read
   */
  void prepare(bnio::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_read(file_.native_handle(), buffer_.data,
                  detail::bounded_io_size(buffer_.size), offset_);
  }

  /**
   * Starts the read operation, rejecting out-of-range offsets inline.
   */
  void start() noexcept {
    if (!detail::valid_random_access_offset(offset_)) {
      this->result = -EOVERFLOW;
      this->flags = 0;
      this->complete_with_ec(-EOVERFLOW);
      (void)this->context_->post(*this);
      return;
    }
    this->start_io(*this);
  }

 private:
  random_access_file file_;
  buffer_view buffer_;
  std::uint64_t offset_;
};

/**
 * Operation representing a positioned io_uring write request on a random
 * access file: the SQE carries the explicit offset, so the kernel file
 * position is neither observed nor advanced. Offsets beyond the kernel's
 * signed loff_t are rejected with EOVERFLOW before entering the kernel.
 */
template <class Receiver>
class io_uring_random_access_write_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a positioned write operation for a random access file buffer.
   */
  io_uring_random_access_write_operation(io_uring_context& context,
                                         random_access_file file,
                                         const buffer_view& buffer,
                                         std::uint64_t offset,
                                         Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        file_(file),
        buffer_(buffer),
        offset_(offset) {
    this->enable_eagain_rearm();
  }

  /**
   * Prepares the write SQE.
   *
   * @see io_uring_prep_write
   */
  void prepare(bnio::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_write(file_.native_handle(), buffer_.data,
                   detail::bounded_io_size(buffer_.size), offset_);
  }

  /**
   * Starts the write operation, rejecting out-of-range offsets inline.
   */
  void start() noexcept {
    if (!detail::valid_random_access_offset(offset_)) {
      this->result = -EOVERFLOW;
      this->flags = 0;
      this->complete_with_ec(-EOVERFLOW);
      (void)this->context_->post(*this);
      return;
    }
    this->start_io(*this);
  }

 private:
  random_access_file file_;
  buffer_view buffer_;
  std::uint64_t offset_;
};

/**
 * Operation representing a positioned io_uring readv request on a random
 * access file: the SQE carries the explicit offset. Offsets beyond the
 * kernel's signed loff_t are rejected with EOVERFLOW before entering the
 * kernel.
 */
template <class Receiver>
class io_uring_random_access_readv_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a positioned readv operation for a random access file and
   * iovec array.
   */
  io_uring_random_access_readv_operation(io_uring_context& context,
                                         random_access_file file,
                                         buffer_sequence_view buffers,
                                         std::uint64_t offset,
                                         Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        file_(file),
        buffers_(buffers),
        offset_(offset) {
    this->enable_eagain_rearm();
  }

  /**
   * Prepares the readv SQE.
   *
   * @see io_uring_prep_readv
   */
  void prepare(bnio::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_readv(file_.native_handle(), buffers_.native_data(),
                   buffers_.size(), offset_);
  }

  /**
   * Starts the readv operation, rejecting out-of-range offsets inline.
   */
  void start() noexcept {
    if (!detail::valid_random_access_offset(offset_)) {
      this->result = -EOVERFLOW;
      this->flags = 0;
      this->complete_with_ec(-EOVERFLOW);
      (void)this->context_->post(*this);
      return;
    }
    this->start_io(*this);
  }

 private:
  random_access_file file_;
  buffer_sequence_view buffers_;
  std::uint64_t offset_;
};

/**
 * Operation representing a positioned io_uring writev request on a random
 * access file: the SQE carries the explicit offset. Offsets beyond the
 * kernel's signed loff_t are rejected with EOVERFLOW before entering the
 * kernel.
 */
template <class Receiver>
class io_uring_random_access_writev_operation
    : public detail::io_uring_receiver_operation<Receiver> {
 public:
  /**
   * Creates a positioned writev operation for a random access file and
   * iovec array.
   */
  io_uring_random_access_writev_operation(io_uring_context& context,
                                          random_access_file file,
                                          buffer_sequence_view buffers,
                                          std::uint64_t offset,
                                          Receiver receiver)
      : detail::io_uring_receiver_operation<Receiver>(context,
                                                      std::move(receiver)),
        file_(file),
        buffers_(buffers),
        offset_(offset) {
    this->enable_eagain_rearm();
  }

  /**
   * Prepares the writev SQE.
   *
   * @see io_uring_prep_writev
   */
  void prepare(bnio::base::submission_queue_entry& sqe) noexcept override {
    sqe.prep_writev(file_.native_handle(), buffers_.native_data(),
                    buffers_.size(), offset_);
  }

  /**
   * Starts the writev operation, rejecting out-of-range offsets inline.
   */
  void start() noexcept {
    if (!detail::valid_random_access_offset(offset_)) {
      this->result = -EOVERFLOW;
      this->flags = 0;
      this->complete_with_ec(-EOVERFLOW);
      (void)this->context_->post(*this);
      return;
    }
    this->start_io(*this);
  }

 private:
  random_access_file file_;
  buffer_sequence_view buffers_;
  std::uint64_t offset_;
};

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_FILE_H_
