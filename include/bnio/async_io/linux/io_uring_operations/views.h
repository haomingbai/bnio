#pragma once
#ifndef BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_VIEWS_H_
#define BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_VIEWS_H_

#include <sys/socket.h>
#include <sys/uio.h>

namespace bnio::async_io::linux_native {

/**
 * Non-owning C++ view over a mutable native socket message.
 *
 * Copying or moving this view copies only the message pointer. The msghdr
 * object remains owned by the caller.
 */
class mutable_message_view {
 public:
  /**
   * Creates an invalid message view.
   */
  constexpr mutable_message_view() noexcept = default;

  /**
   * Wraps a mutable socket message without taking ownership.
   */
  constexpr explicit mutable_message_view(msghdr& message) noexcept
      : message_(&message) {}

  /**
   * Copies a message view without taking ownership.
   */
  constexpr mutable_message_view(const mutable_message_view&) noexcept =
      default;

  /**
   * Copies a message view without taking ownership.
   */
  constexpr mutable_message_view& operator=(
      const mutable_message_view&) noexcept = default;

  /**
   * Moves a message view by copying the message pointer.
   */
  constexpr mutable_message_view(mutable_message_view&&) noexcept = default;

  /**
   * Moves a message view by copying the message pointer.
   */
  constexpr mutable_message_view& operator=(mutable_message_view&&) noexcept =
      default;

  /**
   * Destroys the view without releasing the message.
   */
  ~mutable_message_view() noexcept = default;

  /**
   * Returns the wrapped native message pointer.
   */
  [[nodiscard]] constexpr msghdr* native_handle() const noexcept {
    return message_;
  }

  /**
   * Returns whether this view references a message.
   */
  [[nodiscard]] constexpr bool valid() const noexcept {
    return message_ != nullptr;
  }

 private:
  msghdr* message_ = nullptr;
};

/**
 * Non-owning C++ view over an immutable native socket message.
 *
 * Copying or moving this view copies only the message pointer. The msghdr
 * object remains owned by the caller.
 */
class const_message_view {
 public:
  /**
   * Creates an invalid message view.
   */
  constexpr const_message_view() noexcept = default;

  /**
   * Wraps an immutable socket message without taking ownership.
   */
  constexpr explicit const_message_view(const msghdr& message) noexcept
      : message_(&message) {}

  /**
   * Copies a message view without taking ownership.
   */
  constexpr const_message_view(const const_message_view&) noexcept = default;

  /**
   * Copies a message view without taking ownership.
   */
  constexpr const_message_view& operator=(const const_message_view&) noexcept =
      default;

  /**
   * Moves a message view by copying the message pointer.
   */
  constexpr const_message_view(const_message_view&&) noexcept = default;

  /**
   * Moves a message view by copying the message pointer.
   */
  constexpr const_message_view& operator=(const_message_view&&) noexcept =
      default;

  /**
   * Destroys the view without releasing the message.
   */
  ~const_message_view() noexcept = default;

  /**
   * Returns the wrapped native message pointer.
   */
  [[nodiscard]] constexpr const msghdr* native_handle() const noexcept {
    return message_;
  }

  /**
   * Returns whether this view references a message.
   */
  [[nodiscard]] constexpr bool valid() const noexcept {
    return message_ != nullptr;
  }

 private:
  const msghdr* message_ = nullptr;
};

/**
 * Non-owning C++ view over a native scatter/gather buffer sequence.
 *
 * Copying or moving this view copies only the iovec pointer and element count.
 * The iovec array remains owned by the caller.
 */
class buffer_sequence_view {
 public:
  /**
   * Creates an empty buffer sequence view.
   */
  constexpr buffer_sequence_view() noexcept = default;

  /**
   * Wraps a native scatter/gather buffer sequence without taking ownership.
   */
  constexpr buffer_sequence_view(const iovec* buffers,
                                 unsigned buffer_count) noexcept
      : buffers_(buffers), buffer_count_(buffer_count) {}

  /**
   * Copies a buffer sequence view without taking ownership.
   */
  constexpr buffer_sequence_view(const buffer_sequence_view&) noexcept =
      default;

  /**
   * Copies a buffer sequence view without taking ownership.
   */
  constexpr buffer_sequence_view& operator=(
      const buffer_sequence_view&) noexcept = default;

  /**
   * Moves a buffer sequence view by copying the pointer and count.
   */
  constexpr buffer_sequence_view(buffer_sequence_view&&) noexcept = default;

  /**
   * Moves a buffer sequence view by copying the pointer and count.
   */
  constexpr buffer_sequence_view& operator=(buffer_sequence_view&&) noexcept =
      default;

  /**
   * Destroys the view without releasing the iovec array.
   */
  ~buffer_sequence_view() noexcept = default;

  /**
   * Returns the native scatter/gather buffer sequence.
   */
  [[nodiscard]] constexpr const iovec* native_data() const noexcept {
    return buffers_;
  }

  /**
   * Returns the number of native scatter/gather buffers.
   */
  [[nodiscard]] constexpr unsigned size() const noexcept {
    return buffer_count_;
  }

  /**
   * Returns whether this view references at least one buffer.
   */
  [[nodiscard]] constexpr bool valid() const noexcept {
    return buffers_ != nullptr || buffer_count_ == 0;
  }

 private:
  const iovec* buffers_ = nullptr;
  unsigned buffer_count_ = 0;
};

}  // namespace bnio::async_io::linux_native

#endif  // BNIO_ASYNC_IO_LINUX_IO_URING_OPERATIONS_VIEWS_H_
