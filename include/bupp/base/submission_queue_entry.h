#pragma once
#ifndef BUPP_BASE_SUBMISSION_QUEUE_ENTRY_H_
#define BUPP_BASE_SUBMISSION_QUEUE_ENTRY_H_

#include <bupp/export.h>
#include <liburing.h>

#include <cstddef>
#include <cstdint>

namespace bupp::base {

/**
 * Non-owning wrapper for an io_uring submission queue entry.
 *
 * Copying or moving this wrapper copies only the raw SQE pointer. The entry is
 * owned by the ring until submission.
 *
 * @see io_uring_sqe
 */
class BUPP_EXPORT submission_queue_entry {
 public:
  /**
   * Creates a null submission queue entry wrapper.
   *
   * @see io_uring_sqe
   */
  submission_queue_entry() noexcept;

  /**
   * Wraps a raw submission queue entry pointer.
   *
   * @see io_uring_sqe
   */
  explicit submission_queue_entry(io_uring_sqe* sqe) noexcept;

  /**
   * Copies an SQE wrapper without taking ownership.
   */
  submission_queue_entry(const submission_queue_entry&) noexcept = default;

  /**
   * Copies an SQE wrapper without taking ownership.
   */
  submission_queue_entry& operator=(const submission_queue_entry&) noexcept =
      default;

  /**
   * Moves an SQE wrapper by copying the raw pointer.
   */
  submission_queue_entry(submission_queue_entry&&) noexcept = default;

  /**
   * Moves an SQE wrapper by copying the raw pointer.
   */
  submission_queue_entry& operator=(submission_queue_entry&&) noexcept =
      default;

  /**
   * Destroys the wrapper without submitting or clearing the SQE.
   */
  ~submission_queue_entry() noexcept = default;

  /**
   * Returns the wrapped submission queue entry.
   *
   * @see io_uring_sqe
   */
  [[nodiscard]] io_uring_sqe* raw() noexcept;

  /**
   * Returns the wrapped submission queue entry.
   *
   * @see io_uring_sqe
   */
  [[nodiscard]] const io_uring_sqe* raw() const noexcept;

  /**
   * Associates pointer user data with this SQE.
   *
   * @see io_uring_sqe_set_data
   */
  void set_data(void* data) noexcept;

  /**
   * Associates 64-bit user data with this SQE.
   *
   * @see io_uring_sqe_set_data64
   */
  void set_data64(std::uint64_t data) noexcept;

  /**
   * Sets SQE flags.
   *
   * @see io_uring_sqe_set_flags
   */
  void set_flags(unsigned flags) noexcept;

  /**
   * Sets the provided buffer group ID.
   *
   * @see io_uring_sqe_set_buf_group
   */
  void set_buf_group(int bgid) noexcept;

  /**
   * Prepares a no-op operation.
   *
   * @see io_uring_prep_nop
   */
  void prep_nop() noexcept;

  /**
   * Prepares an accept operation.
   *
   * @see io_uring_prep_accept
   */
  void prep_accept(int fd, sockaddr* addr, socklen_t* addrlen,
                   int flags) noexcept;

  /**
   * Prepares a connect operation.
   *
   * @see io_uring_prep_connect
   */
  void prep_connect(int fd, const sockaddr* addr, socklen_t addrlen) noexcept;

  /**
   * Prepares a send operation.
   *
   * @see io_uring_prep_send
   */
  void prep_send(int sockfd, const void* buf, std::size_t len,
                 int flags) noexcept;

  /**
   * Prepares a sendmsg operation.
   *
   * @see io_uring_prep_sendmsg
   */
  void prep_sendmsg(int fd, const msghdr* msg, unsigned flags) noexcept;

  /**
   * Prepares a recv operation.
   *
   * @see io_uring_prep_recv
   */
  void prep_recv(int sockfd, void* buf, std::size_t len, int flags) noexcept;

  /**
   * Prepares a recvmsg operation.
   *
   * @see io_uring_prep_recvmsg
   */
  void prep_recvmsg(int fd, msghdr* msg, unsigned flags) noexcept;

  /**
   * Prepares a poll add operation.
   *
   * @see io_uring_prep_poll_add
   */
  void prep_poll_add(int fd, unsigned poll_mask) noexcept;

  /**
   * Prepares a multishot poll add operation.
   *
   * @see io_uring_prep_poll_multishot
   */
  void prep_poll_multishot(int fd, unsigned poll_mask) noexcept;

  /**
   * Prepares a poll remove operation.
   *
   * @see io_uring_prep_poll_remove
   */
  void prep_poll_remove(std::uint64_t user_data) noexcept;

  /**
   * Prepares a poll update operation.
   *
   * @see io_uring_prep_poll_update
   */
  void prep_poll_update(std::uint64_t old_user_data,
                        std::uint64_t new_user_data, unsigned poll_mask,
                        unsigned flags) noexcept;

  /**
   * Prepares a timeout operation.
   *
   * @see io_uring_prep_timeout
   */
  void prep_timeout(const __kernel_timespec* ts, unsigned count,
                    unsigned flags) noexcept;

  /**
   * Prepares a timeout remove operation.
   *
   * @see io_uring_prep_timeout_remove
   */
  void prep_timeout_remove(std::uint64_t user_data, unsigned flags) noexcept;

  /**
   * Prepares a timeout update operation.
   *
   * @see io_uring_prep_timeout_update
   */
  void prep_timeout_update(const __kernel_timespec* ts, std::uint64_t user_data,
                           unsigned flags) noexcept;

  /**
   * Prepares an async cancel operation using pointer user data.
   *
   * @see io_uring_prep_cancel
   */
  void prep_cancel(const void* user_data, int flags) noexcept;

  /**
   * Prepares an async cancel operation using 64-bit user data.
   *
   * @see io_uring_prep_cancel64
   */
  void prep_cancel64(std::uint64_t user_data, int flags) noexcept;

  /**
   * Prepares an async cancel operation matching a file descriptor.
   *
   * @see io_uring_prep_cancel_fd
   */
  void prep_cancel_fd(int fd, unsigned flags) noexcept;

  /**
   * Prepares a read operation.
   *
   * @see io_uring_prep_read
   */
  void prep_read(int fd, void* buf, unsigned nbytes,
                 std::uint64_t offset) noexcept;

  /**
   * Prepares a write operation.
   *
   * @see io_uring_prep_write
   */
  void prep_write(int fd, const void* buf, unsigned nbytes,
                  std::uint64_t offset) noexcept;

  /**
   * Prepares a readv operation.
   *
   * @see io_uring_prep_readv
   */
  void prep_readv(int fd, const iovec* iovecs, unsigned nr_vecs,
                  std::uint64_t offset) noexcept;

  /**
   * Prepares a writev operation.
   *
   * @see io_uring_prep_writev
   */
  void prep_writev(int fd, const iovec* iovecs, unsigned nr_vecs,
                   std::uint64_t offset) noexcept;

  /**
   * Prepares an openat operation.
   *
   * @see io_uring_prep_openat
   */
  void prep_openat(int dfd, const char* path, int flags, mode_t mode) noexcept;

  /**
   * Prepares an open operation.
   *
   * @see io_uring_prep_open
   */
  void prep_open(const char* path, int flags, mode_t mode) noexcept;

  /**
   * Prepares a close operation.
   *
   * @see io_uring_prep_close
   */
  void prep_close(int fd) noexcept;

  /**
   * Prepares an fsync operation.
   *
   * @see io_uring_prep_fsync
   */
  void prep_fsync(int fd, unsigned fsync_flags) noexcept;

  /**
   * Prepares a statx operation.
   *
   * @see io_uring_prep_statx
   */
  void prep_statx(int dfd, const char* path, int flags, unsigned mask,
                  struct statx* statxbuf) noexcept;

  /**
   * Prepares a fallocate operation.
   *
   * @see io_uring_prep_fallocate
   */
  void prep_fallocate(int fd, int mode, std::uint64_t offset,
                      std::uint64_t len) noexcept;

  /**
   * Prepares a provided buffers operation.
   *
   * @see io_uring_prep_provide_buffers
   */
  void prep_provide_buffers(void* addr, int len, int nr, int bgid,
                            int bid) noexcept;

  /**
   * Prepares a remove buffers operation.
   *
   * @see io_uring_prep_remove_buffers
   */
  void prep_remove_buffers(int nr, int bgid) noexcept;

 private:
  io_uring_sqe* sqe_ = nullptr;
};

}  // namespace bupp::base

#endif  // BUPP_BASE_SUBMISSION_QUEUE_ENTRY_H_
