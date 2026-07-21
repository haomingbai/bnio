/**
 * @file dynamic_byte_vector.h
 * @brief Dynamic buffer backed by std::vector<std::byte>.
 */

#pragma once
#ifndef BNIO_BUFFER_DYNAMIC_BYTE_VECTOR_H_
#define BNIO_BUFFER_DYNAMIC_BYTE_VECTOR_H_

#include <bnio/buffer/basic.h>

#include <cstddef>
#include <vector>

namespace bnio {

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
 * Creates a dynamic buffer adapter backed by a byte vector.
 */
template <class Allocator>
[[nodiscard]] inline dynamic_byte_vector_buffer<Allocator> dynamic_buffer(
    std::vector<std::byte, Allocator>& storage) noexcept {
  return dynamic_byte_vector_buffer<Allocator>(storage);
}

}  // namespace bnio

#endif  // BNIO_BUFFER_DYNAMIC_BYTE_VECTOR_H_
