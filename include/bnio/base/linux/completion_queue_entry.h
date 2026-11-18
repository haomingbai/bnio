/**
 * @file completion_queue_entry.h
 * @brief Wrapper for an io_uring completion queue entry.
 */

#pragma once
#ifndef BNIO_BASE_LINUX_COMPLETION_QUEUE_ENTRY_H_
#define BNIO_BASE_LINUX_COMPLETION_QUEUE_ENTRY_H_

#include <bnio/export.h>

#include <cstdint>

struct io_uring_cqe;

namespace bnio::base {

/**
 * Non-owning io_uring_cqe wrapper; copies share the ring-owned pointer, which
 * stays valid until the entry is marked seen.
 */
class BNIO_EXPORT completion_queue_entry {
 public:
  /** Creates a null completion queue entry wrapper. */
  completion_queue_entry() noexcept;

  /** Wraps @p cqe without taking ownership. */
  explicit completion_queue_entry(io_uring_cqe* cqe) noexcept;

  /** Copies the CQE pointer; ownership stays with the ring. */
  completion_queue_entry(const completion_queue_entry&) noexcept = default;

  /** Copy assigns the CQE pointer. */
  completion_queue_entry& operator=(const completion_queue_entry&) noexcept =
      default;

  /** Moves the CQE pointer; ownership stays with the ring. */
  completion_queue_entry(completion_queue_entry&&) noexcept = default;

  /** Move assigns the CQE pointer. */
  completion_queue_entry& operator=(completion_queue_entry&&) noexcept =
      default;

  /** Destroys the wrapper without acknowledging the CQE. */
  ~completion_queue_entry() noexcept = default;

  /** Returns the wrapped CQE pointer. */
  [[nodiscard]] io_uring_cqe* raw() noexcept;

  /** Const overload. */
  [[nodiscard]] const io_uring_cqe* raw() const noexcept;

  /** Completion result code. */
  [[nodiscard]] int res() const noexcept;

  /** Completion flags. */
  [[nodiscard]] unsigned flags() const noexcept;

  /** Pointer user data associated with the submitted SQE. */
  [[nodiscard]] void* get_data() const noexcept;

  /** 64-bit user data associated with the submitted SQE. */
  [[nodiscard]] std::uint64_t get_data64() const noexcept;

 private:
  io_uring_cqe* cqe_ = nullptr;
};

}  // namespace bnio::base

#endif  // BNIO_BASE_LINUX_COMPLETION_QUEUE_ENTRY_H_
