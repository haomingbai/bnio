/**
 * @file basic.h
 * @brief Non-owning mutable and constant buffer views.
 */

#pragma once
#ifndef BNIO_BUFFER_BASIC_H_
#define BNIO_BUFFER_BASIC_H_

#include <bnio/async_io/buffer_view.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bnio {

/**
 * Non-owning view over mutable contiguous bytes.
 *
 * Copying or moving this object copies only the pointer and size. The
 * pointed-to storage remains owned by the caller.
 */
class mutable_buffer {
 public:
  /**
   * Creates an empty mutable buffer view.
   */
  constexpr mutable_buffer() noexcept = default;

  /**
   * Creates a mutable buffer view over a contiguous byte range.
   */
  constexpr mutable_buffer(void* data, std::size_t size) noexcept
      : data_(data), size_(size) {}

  /**
   * Copies a mutable buffer view without taking ownership.
   */
  constexpr mutable_buffer(const mutable_buffer&) noexcept = default;

  /**
   * Copies a mutable buffer view without taking ownership.
   */
  constexpr mutable_buffer& operator=(const mutable_buffer&) noexcept = default;

  /**
   * Moves a mutable buffer view by copying the pointer and size.
   */
  constexpr mutable_buffer(mutable_buffer&&) noexcept = default;

  /**
   * Moves a mutable buffer view by copying the pointer and size.
   */
  constexpr mutable_buffer& operator=(mutable_buffer&&) noexcept = default;

  /**
   * Destroys the view without releasing storage.
   */
  ~mutable_buffer() noexcept = default;

  /**
   * Returns the first byte in the referenced range.
   */
  [[nodiscard]] constexpr void* data() const noexcept { return data_; }

  /**
   * Returns the number of bytes in the referenced range.
   */
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }

  /**
   * Converts this buffer to the async_io non-owning buffer view.
   */
  [[nodiscard]] constexpr async_io::buffer_view view() const noexcept {
    return {data_, size_};
  }

 private:
  void* data_ = nullptr;
  std::size_t size_ = 0;
};

/**
 * Non-owning view over immutable contiguous bytes.
 *
 * Copying or moving this object copies only the pointer and size. The
 * pointed-to storage remains owned by the caller.
 */
class const_buffer {
 public:
  /**
   * Creates an empty immutable buffer view.
   */
  constexpr const_buffer() noexcept = default;

  /**
   * Creates an immutable buffer view over a contiguous byte range.
   */
  constexpr const_buffer(const void* data, std::size_t size) noexcept
      : data_(data), size_(size) {}

  /**
   * Copies an immutable buffer view without taking ownership.
   */
  constexpr const_buffer(const const_buffer&) noexcept = default;

  /**
   * Copies an immutable buffer view without taking ownership.
   */
  constexpr const_buffer& operator=(const const_buffer&) noexcept = default;

  /**
   * Moves an immutable buffer view by copying the pointer and size.
   */
  constexpr const_buffer(const_buffer&&) noexcept = default;

  /**
   * Moves an immutable buffer view by copying the pointer and size.
   */
  constexpr const_buffer& operator=(const_buffer&&) noexcept = default;

  /**
   * Destroys the view without releasing storage.
   */
  ~const_buffer() noexcept = default;

  /**
   * Returns the first byte in the referenced range.
   */
  [[nodiscard]] constexpr const void* data() const noexcept { return data_; }

  /**
   * Returns the number of bytes in the referenced range.
   */
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }

 private:
  const void* data_ = nullptr;
  std::size_t size_ = 0;
};

/**
 * Creates a mutable buffer view from a pointer and byte count.
 */
[[nodiscard]] constexpr mutable_buffer buffer(void* data,
                                              std::size_t size) noexcept {
  return {data, size};
}

/**
 * Creates an immutable buffer view from a pointer and byte count.
 */
[[nodiscard]] constexpr const_buffer buffer(const void* data,
                                            std::size_t size) noexcept {
  return {data, size};
}

/**
 * Creates a mutable buffer view from an async_io buffer view.
 */
[[nodiscard]] constexpr mutable_buffer buffer(
    async_io::buffer_view view) noexcept {
  return {view.data, view.size};
}

/**
 * Creates a mutable buffer view over a mutable span.
 */
template <class T, std::size_t Extent>
[[nodiscard]] constexpr mutable_buffer buffer(
    std::span<T, Extent> data) noexcept
  requires(!std::is_const_v<T>)
{
  return {data.data(), data.size_bytes()};
}

/**
 * Creates an immutable buffer view over a const span.
 */
template <class T, std::size_t Extent>
[[nodiscard]] constexpr const_buffer buffer(
    std::span<const T, Extent> data) noexcept {
  return {data.data(), data.size_bytes()};
}

/**
 * Creates a mutable buffer view over a mutable array.
 */
template <class T, std::size_t Size>
[[nodiscard]] constexpr mutable_buffer buffer(
    std::array<T, Size>& data) noexcept
  requires(!std::is_const_v<T>)
{
  return {data.data(), sizeof(T) * data.size()};
}

/**
 * Creates an immutable buffer view over an array.
 */
template <class T, std::size_t Size>
[[nodiscard]] constexpr const_buffer buffer(
    const std::array<T, Size>& data) noexcept {
  return {data.data(), sizeof(T) * data.size()};
}

/**
 * Creates a mutable buffer view over a mutable vector.
 */
template <class T, class Allocator>
[[nodiscard]] mutable_buffer buffer(std::vector<T, Allocator>& data) noexcept
  requires(!std::is_const_v<T>)
{
  return {data.data(), sizeof(T) * data.size()};
}

/**
 * Creates an immutable buffer view over a vector.
 */
template <class T, class Allocator>
[[nodiscard]] const_buffer buffer(
    const std::vector<T, Allocator>& data) noexcept {
  return {data.data(), sizeof(T) * data.size()};
}

/**
 * Creates a mutable buffer view over a string's character storage.
 */
[[nodiscard]] inline mutable_buffer buffer(std::string& data) noexcept {
  return {data.data(), data.size()};
}

/**
 * Creates an immutable buffer view over a string's character storage.
 */
[[nodiscard]] inline const_buffer buffer(const std::string& data) noexcept {
  return {data.data(), data.size()};
}

/**
 * Creates an immutable buffer view over a string_view.
 */
[[nodiscard]] constexpr const_buffer buffer(std::string_view data) noexcept {
  return {data.data(), data.size()};
}

}  // namespace bnio

#endif  // BNIO_BUFFER_BASIC_H_
