#pragma once
#ifndef BUPP_BUFFER_H_
#define BUPP_BUFFER_H_

#include <bupp/async_io/buffer_view.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bupp {

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

/**
 * Dynamic buffer adapter backed by a caller-owned std::string.
 *
 * The adapter never owns the string. Copying or moving the adapter copies the
 * string pointer and prepared-region bookkeeping.
 */
class dynamic_string_buffer {
 public:
  /**
   * Creates a dynamic buffer adapter for a string.
   */
  explicit dynamic_string_buffer(std::string& storage) noexcept
      : storage_(&storage) {}

  /**
   * Copies the adapter without taking ownership of the string.
   */
  dynamic_string_buffer(const dynamic_string_buffer&) noexcept = default;

  /**
   * Copies the adapter without taking ownership of the string.
   */
  dynamic_string_buffer& operator=(const dynamic_string_buffer&) noexcept =
      default;

  /**
   * Moves the adapter by copying its string pointer and bookkeeping.
   */
  dynamic_string_buffer(dynamic_string_buffer&&) noexcept = default;

  /**
   * Moves the adapter by copying its string pointer and bookkeeping.
   */
  dynamic_string_buffer& operator=(dynamic_string_buffer&&) noexcept = default;

  /**
   * Destroys the adapter without modifying ownership of the string.
   */
  ~dynamic_string_buffer() noexcept = default;

  /**
   * Returns the current committed byte count.
   */
  [[nodiscard]] std::size_t size() const noexcept { return storage_->size(); }

  /**
   * Returns an immutable view over committed bytes.
   */
  [[nodiscard]] const_buffer data() const noexcept {
    return {storage_->data(), storage_->size()};
  }

  /**
   * Grows the string and returns the newly prepared mutable region.
   */
  [[nodiscard]] mutable_buffer prepare(std::size_t size) {
    prepared_offset_ = storage_->size();
    storage_->resize(prepared_offset_ + size);
    prepared_size_ = size;
    return {storage_->data() + prepared_offset_, prepared_size_};
  }

  /**
   * Commits bytes from the most recently prepared region.
   */
  void commit(std::size_t size) {
    if (size < prepared_size_) {
      storage_->resize(prepared_offset_ + size);
    }
    prepared_size_ = 0;
  }

  /**
   * Removes bytes from the front of the string.
   */
  void consume(std::size_t size) {
    if (size >= storage_->size()) {
      storage_->clear();
      return;
    }
    storage_->erase(0, size);
  }

 private:
  std::string* storage_;
  std::size_t prepared_offset_ = 0;
  std::size_t prepared_size_ = 0;
};

/**
 * Dynamic buffer adapter backed by a caller-owned std::vector<std::byte>.
 *
 * The adapter never owns the vector. Copying or moving the adapter copies the
 * vector pointer and prepared-region bookkeeping.
 */
template <class Allocator = std::allocator<std::byte>>
class dynamic_byte_vector_buffer {
 public:
  /**
   * Creates a dynamic buffer adapter for a byte vector.
   */
  explicit dynamic_byte_vector_buffer(
      std::vector<std::byte, Allocator>& storage) noexcept
      : storage_(&storage) {}

  /**
   * Copies the adapter without taking ownership of the vector.
   */
  dynamic_byte_vector_buffer(const dynamic_byte_vector_buffer&) noexcept =
      default;

  /**
   * Copies the adapter without taking ownership of the vector.
   */
  dynamic_byte_vector_buffer& operator=(
      const dynamic_byte_vector_buffer&) noexcept = default;

  /**
   * Moves the adapter by copying its vector pointer and bookkeeping.
   */
  dynamic_byte_vector_buffer(dynamic_byte_vector_buffer&&) noexcept = default;

  /**
   * Moves the adapter by copying its vector pointer and bookkeeping.
   */
  dynamic_byte_vector_buffer& operator=(dynamic_byte_vector_buffer&&) noexcept =
      default;

  /**
   * Destroys the adapter without modifying ownership of the vector.
   */
  ~dynamic_byte_vector_buffer() noexcept = default;

  /**
   * Returns the current committed byte count.
   */
  [[nodiscard]] std::size_t size() const noexcept { return storage_->size(); }

  /**
   * Returns an immutable view over committed bytes.
   */
  [[nodiscard]] const_buffer data() const noexcept {
    return {storage_->data(), storage_->size()};
  }

  /**
   * Grows the vector and returns the newly prepared mutable region.
   */
  [[nodiscard]] mutable_buffer prepare(std::size_t size) {
    prepared_offset_ = storage_->size();
    storage_->resize(prepared_offset_ + size);
    prepared_size_ = size;
    return {storage_->data() + prepared_offset_, prepared_size_};
  }

  /**
   * Commits bytes from the most recently prepared region.
   */
  void commit(std::size_t size) {
    if (size < prepared_size_) {
      storage_->resize(prepared_offset_ + size);
    }
    prepared_size_ = 0;
  }

  /**
   * Removes bytes from the front of the vector.
   */
  void consume(std::size_t size) {
    if (size >= storage_->size()) {
      storage_->clear();
      return;
    }
    storage_->erase(storage_->begin(),
                    storage_->begin() + static_cast<std::ptrdiff_t>(size));
  }

 private:
  std::vector<std::byte, Allocator>* storage_;
  std::size_t prepared_offset_ = 0;
  std::size_t prepared_size_ = 0;
};

/**
 * Creates a dynamic buffer adapter backed by a string.
 */
[[nodiscard]] inline dynamic_string_buffer dynamic_buffer(
    std::string& storage) noexcept {
  return dynamic_string_buffer(storage);
}

/**
 * Creates a dynamic buffer adapter backed by a byte vector.
 */
template <class Allocator>
[[nodiscard]] inline dynamic_byte_vector_buffer<Allocator> dynamic_buffer(
    std::vector<std::byte, Allocator>& storage) noexcept {
  return dynamic_byte_vector_buffer<Allocator>(storage);
}

/** @cond BUPP_DETAIL */
namespace detail {

template <class T>
concept dynamic_buffer_like = requires(T buffer, std::size_t size) {
  { buffer.prepare(size) } -> std::same_as<mutable_buffer>;
  buffer.commit(size);
  { buffer.data() } -> std::same_as<const_buffer>;
  { buffer.size() } -> std::convertible_to<std::size_t>;
};

class mutable_buffer_holder {
 public:
  explicit mutable_buffer_holder(mutable_buffer buffer) noexcept
      : buffer_(buffer) {}

  [[nodiscard]] async_io::buffer_view view() const noexcept {
    return buffer_.view();
  }

  void commit(std::size_t) noexcept {}

 private:
  mutable_buffer buffer_;
};

class const_buffer_holder {
 public:
  explicit const_buffer_holder(const_buffer buffer) noexcept
      : buffer_(buffer) {}

  [[nodiscard]] const void* data() const noexcept { return buffer_.data(); }

  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

 private:
  const_buffer buffer_;
};

template <dynamic_buffer_like DynamicBuffer>
class dynamic_buffer_holder {
 public:
  explicit dynamic_buffer_holder(DynamicBuffer buffer,
                                 std::size_t prepare_size = 4096)
      : buffer_(buffer), prepared_(buffer_.prepare(prepare_size)) {}

  [[nodiscard]] async_io::buffer_view view() const noexcept {
    return prepared_.view();
  }

  void commit(std::size_t size) { buffer_.commit(size); }

 private:
  DynamicBuffer buffer_;
  mutable_buffer prepared_;
};

template <class Buffer>
[[nodiscard]] auto make_mutable_buffer_holder(Buffer&& value) {
  using buffer_type = std::remove_cvref_t<Buffer>;
  if constexpr (std::same_as<buffer_type, mutable_buffer>) {
    return mutable_buffer_holder(value);
  } else if constexpr (std::same_as<buffer_type, async_io::buffer_view>) {
    return mutable_buffer_holder(buffer(value));
  } else if constexpr (dynamic_buffer_like<buffer_type>) {
    return dynamic_buffer_holder<buffer_type>(value);
  } else {
    return mutable_buffer_holder(buffer(value));
  }
}

template <class Buffer>
[[nodiscard]] auto make_const_buffer_holder(Buffer&& value) {
  using buffer_type = std::remove_cvref_t<Buffer>;
  if constexpr (std::same_as<buffer_type, const_buffer>) {
    return const_buffer_holder(value);
  } else if constexpr (std::same_as<buffer_type, mutable_buffer>) {
    return const_buffer_holder({value.data(), value.size()});
  } else if constexpr (std::same_as<buffer_type, async_io::buffer_view>) {
    return const_buffer_holder({value.data, value.size});
  } else if constexpr (dynamic_buffer_like<buffer_type>) {
    const_buffer data = value.data();
    return const_buffer_holder(data);
  } else {
    return const_buffer_holder(buffer(value));
  }
}

}  // namespace detail
/** @endcond */

}  // namespace bupp

#endif  // BUPP_BUFFER_H_
