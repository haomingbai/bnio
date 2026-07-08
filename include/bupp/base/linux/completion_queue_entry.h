#pragma once
#ifndef BUPP_BASE_LINUX_COMPLETION_QUEUE_ENTRY_H_
#define BUPP_BASE_LINUX_COMPLETION_QUEUE_ENTRY_H_

#include <bupp/base/linux/liburing.h>
#include <bupp/export.h>

#include <cstdint>

namespace bupp::base {

/**
 * Non-owning wrapper for an io_uring completion queue entry.
 *
 * Copying or moving this wrapper copies only the raw CQE pointer. The entry is
 * owned by the ring until it is marked seen.
 *
 * @see io_uring_cqe
 */
class BUPP_EXPORT completion_queue_entry {
 public:
  /**
   * Creates a null completion queue entry wrapper.
   *
   * @see io_uring_cqe
   */
  completion_queue_entry() noexcept;

  /**
   * Wraps a raw completion queue entry pointer.
   *
   * @see io_uring_cqe
   */
  explicit completion_queue_entry(io_uring_cqe* cqe) noexcept;

  /**
   * Copies a CQE wrapper without taking ownership.
   */
  completion_queue_entry(const completion_queue_entry&) noexcept = default;

  /**
   * Copies a CQE wrapper without taking ownership.
   */
  completion_queue_entry& operator=(const completion_queue_entry&) noexcept =
      default;

  /**
   * Moves a CQE wrapper by copying the raw pointer.
   */
  completion_queue_entry(completion_queue_entry&&) noexcept = default;

  /**
   * Moves a CQE wrapper by copying the raw pointer.
   */
  completion_queue_entry& operator=(completion_queue_entry&&) noexcept =
      default;

  /**
   * Destroys the wrapper without acknowledging the CQE.
   */
  ~completion_queue_entry() noexcept = default;

  /**
   * Returns the wrapped completion queue entry.
   *
   * @see io_uring_cqe
   */
  [[nodiscard]] io_uring_cqe* raw() noexcept;

  /**
   * Returns the wrapped completion queue entry.
   *
   * @see io_uring_cqe
   */
  [[nodiscard]] const io_uring_cqe* raw() const noexcept;

  /**
   * Returns the completion result code.
   *
   * @see io_uring_cqe
   */
  [[nodiscard]] int res() const noexcept;

  /**
   * Returns the completion flags.
   *
   * @see io_uring_cqe
   */
  [[nodiscard]] unsigned flags() const noexcept;

  /**
   * Returns pointer user data associated with the submitted SQE.
   *
   * @see io_uring_cqe_get_data
   */
  [[nodiscard]] void* get_data() const noexcept;

  /**
   * Returns 64-bit user data associated with the submitted SQE.
   *
   * @see io_uring_cqe_get_data64
   */
  [[nodiscard]] std::uint64_t get_data64() const noexcept;

 private:
  io_uring_cqe* cqe_ = nullptr;
};

}  // namespace bupp::base

#endif  // BUPP_BASE_LINUX_COMPLETION_QUEUE_ENTRY_H_
