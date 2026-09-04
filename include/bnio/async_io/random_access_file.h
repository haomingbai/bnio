/**
 * @file random_access_file.h
 * @brief Non-owning view of a random access file descriptor.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_RANDOM_ACCESS_FILE_H_
#define BNIO_ASYNC_IO_RANDOM_ACCESS_FILE_H_

#include <bnio/async_io/descriptor_view.h>

#include <type_traits>

namespace bnio::async_io {

/**
 * Non-owning view over a native file descriptor opened for random access.
 *
 * Unlike descriptor_view, every I/O operation on this view takes an explicit
 * offset and does not observe or advance the kernel file position.
 *
 * Copying or moving this view copies only the descriptor value. The descriptor
 * remains owned and closed by the caller.
 */
class random_access_file {
 public:
  /**
   * Native file descriptor type.
   */
  using native_handle_type = int;

  /**
   * Creates an invalid random access file view.
   */
  constexpr random_access_file() noexcept = default;

  /**
   * Wraps a native file descriptor without taking ownership.
   */
  constexpr explicit random_access_file(native_handle_type fd) noexcept
      : fd_(fd) {}

  /**
   * Reinterprets a descriptor view as a random access file without taking
   * ownership.
   */
  constexpr explicit random_access_file(descriptor_view descriptor) noexcept
      : fd_(descriptor.native_handle()) {}

  /**
   * Copies a random access file view without taking ownership.
   */
  constexpr random_access_file(const random_access_file&) noexcept = default;

  /**
   * Copies a random access file view without taking ownership.
   */
  constexpr random_access_file& operator=(const random_access_file&) noexcept =
      default;

  /**
   * Moves a random access file view by copying the descriptor value.
   */
  constexpr random_access_file(random_access_file&&) noexcept = default;

  /**
   * Moves a random access file view by copying the descriptor value.
   */
  constexpr random_access_file& operator=(random_access_file&&) noexcept =
      default;

  /**
   * Destroys the view without closing the descriptor.
   */
  ~random_access_file() noexcept = default;

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

static_assert(std::is_trivially_copyable_v<random_access_file>,
              "random_access_file must remain trivially copyable");

}  // namespace bnio::async_io

#endif  // BNIO_ASYNC_IO_RANDOM_ACCESS_FILE_H_
