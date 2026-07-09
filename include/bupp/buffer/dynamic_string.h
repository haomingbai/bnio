#pragma once
#ifndef BUPP_BUFFER_DYNAMIC_STRING_H_
#define BUPP_BUFFER_DYNAMIC_STRING_H_

#include <bupp/buffer/basic.h>

#include <cstddef>
#include <string>

namespace bupp {

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
 * Creates a dynamic buffer adapter backed by a string.
 */
[[nodiscard]] inline dynamic_string_buffer dynamic_buffer(
    std::string& storage) noexcept {
  return dynamic_string_buffer(storage);
}

}  // namespace bupp

#endif  // BUPP_BUFFER_DYNAMIC_STRING_H_
