#pragma once
#ifndef BNIO_ASYNC_IO_DESCRIPTOR_VIEW_H_
#define BNIO_ASYNC_IO_DESCRIPTOR_VIEW_H_

namespace bnio::async_io {

/**
 * Non-owning view over a native file descriptor.
 *
 * Copying or moving this view copies only the descriptor value. The descriptor
 * remains owned and closed by the caller.
 */
class descriptor_view {
 public:
  /**
   * Native file descriptor type.
   */
  using native_handle_type = int;

  /**
   * Creates an invalid descriptor view.
   */
  constexpr descriptor_view() noexcept = default;

  /**
   * Wraps a native file descriptor without taking ownership.
   */
  constexpr explicit descriptor_view(native_handle_type fd) noexcept
      : fd_(fd) {}

  /**
   * Copies a descriptor view without taking ownership.
   */
  constexpr descriptor_view(const descriptor_view&) noexcept = default;

  /**
   * Copies a descriptor view without taking ownership.
   */
  constexpr descriptor_view& operator=(const descriptor_view&) noexcept =
      default;

  /**
   * Moves a descriptor view by copying the descriptor value.
   */
  constexpr descriptor_view(descriptor_view&&) noexcept = default;

  /**
   * Moves a descriptor view by copying the descriptor value.
   */
  constexpr descriptor_view& operator=(descriptor_view&&) noexcept = default;

  /**
   * Destroys the view without closing the descriptor.
   */
  ~descriptor_view() noexcept = default;

  /**
   * Returns the wrapped native file descriptor.
   */
  [[nodiscard]] constexpr native_handle_type native_handle() const noexcept {
    return fd_;
  }

  /**
   * Returns whether this view references a valid descriptor value.
   */
  [[nodiscard]] constexpr bool valid() const noexcept { return fd_ >= 0; }

 private:
  native_handle_type fd_ = -1;
};

}  // namespace bnio::async_io

#endif  // BNIO_ASYNC_IO_DESCRIPTOR_VIEW_H_
