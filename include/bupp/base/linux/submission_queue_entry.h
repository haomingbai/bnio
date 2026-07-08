#pragma once
#ifndef BUPP_BASE_LINUX_SUBMISSION_QUEUE_ENTRY_H_
#define BUPP_BASE_LINUX_SUBMISSION_QUEUE_ENTRY_H_

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
   * Prepares a timeout operation.
   *
   * @see io_uring_prep_timeout
   */
  void prep_timeout(__kernel_timespec* ts, unsigned count,
                    unsigned flags) noexcept;

  /**
   * Prepares a timeout update operation.
   *
   * @see io_uring_prep_timeout_update
   */
  void prep_timeout_update(__kernel_timespec* ts, std::uint64_t user_data,
                           unsigned flags) noexcept;

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

 private:
  io_uring_sqe* sqe_ = nullptr;
};

}  // namespace bupp::base

#endif  // BUPP_BASE_LINUX_SUBMISSION_QUEUE_ENTRY_H_
