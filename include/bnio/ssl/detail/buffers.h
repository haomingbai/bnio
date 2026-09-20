/**
 * @file buffers.h
 * @brief Borrow-level buffer adapters for SSL async operations.
 */

#pragma once
#ifndef BNIO_SSL_DETAIL_BUFFERS_H_
#define BNIO_SSL_DETAIL_BUFFERS_H_

#include <bnio/buffer/basic.h>

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace bnio::ssl {

/** @cond BNIO_DETAIL */
namespace detail {

template <class T>
concept dynamic_buffer_like = requires(T buffer, std::size_t size) {
  { buffer.prepare(size) } -> std::same_as<mutable_buffer>;
  buffer.commit(size);
  { buffer.data() } -> std::same_as<const_buffer>;
  { buffer.size() } -> std::convertible_to<std::size_t>;
};

/**
 * Read-side borrow of a user-supplied buffer.
 *
 * A mutable_buffer is borrowed directly; a dynamic buffer is stored by value
 * and prepared with a fixed reserve so SSL_read can write into the prepared
 * window. Either way data()/size() expose the (pointer, size) pair SSL_read
 * needs and commit() records how many bytes were received (a no-op for a
 * static borrow, a forward to the user buffer's commit for a dynamic one).
 */
template <class Buffer>
  requires std::same_as<Buffer, mutable_buffer> ||
           dynamic_buffer_like<Buffer>
class read_buffer_borrow {
 public:
  explicit read_buffer_borrow(Buffer buffer) : buffer_(std::move(buffer)) {
    if constexpr (dynamic_buffer_like<Buffer>) {
      prepared_ = buffer_.prepare(k_prepare_size);
    }
  }

  [[nodiscard]] void* data() const noexcept {
    if constexpr (dynamic_buffer_like<Buffer>) {
      return prepared_.data();
    } else {
      return buffer_.data();
    }
  }

  [[nodiscard]] std::size_t size() const noexcept {
    if constexpr (dynamic_buffer_like<Buffer>) {
      return prepared_.size();
    } else {
      return buffer_.size();
    }
  }

  void commit(std::size_t size) noexcept(!dynamic_buffer_like<Buffer>) {
    if constexpr (dynamic_buffer_like<Buffer>) {
      buffer_.commit(size);
    }
  }

 private:
  static constexpr std::size_t k_prepare_size = 4096;

  Buffer buffer_;
  mutable_buffer prepared_;
};

/**
 * Write-side borrow of a user-supplied buffer.
 *
 * Borrowed from a const_buffer or a mutable_buffer (normalized to read-only);
 * data()/size() expose the (pointer, size) pair SSL_write needs. No commit
 * exists: writes never hand ownership back.
 */
class write_buffer_borrow {
 public:
  write_buffer_borrow(const_buffer buffer) noexcept : buffer_(buffer) {}

  write_buffer_borrow(mutable_buffer buffer) noexcept
      : buffer_(buffer.data(), buffer.size()) {}

  [[nodiscard]] const void* data() const noexcept { return buffer_.data(); }

  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

 private:
  const_buffer buffer_;
};

template <class Buffer>
[[nodiscard]] auto make_read_buffer(Buffer&& value) {
  using buffer_type = std::remove_cvref_t<Buffer>;
  if constexpr (std::same_as<buffer_type, mutable_buffer>) {
    return read_buffer_borrow<buffer_type>(value);
  } else if constexpr (dynamic_buffer_like<buffer_type>) {
    return read_buffer_borrow<buffer_type>(value);
  } else {
    return read_buffer_borrow<mutable_buffer>(bnio::buffer(value));
  }
}

template <class Buffer>
[[nodiscard]] auto make_write_buffer(Buffer&& value) {
  using buffer_type = std::remove_cvref_t<Buffer>;
  if constexpr (std::same_as<buffer_type, const_buffer>) {
    return write_buffer_borrow(value);
  } else if constexpr (std::same_as<buffer_type, mutable_buffer>) {
    return write_buffer_borrow(value);
  } else if constexpr (dynamic_buffer_like<buffer_type>) {
    return write_buffer_borrow(value.data());
  } else {
    auto data = bnio::buffer(value);
    if constexpr (std::same_as<decltype(data), mutable_buffer>) {
      return write_buffer_borrow(const_buffer(data.data(), data.size()));
    } else {
      return write_buffer_borrow(data);
    }
  }
}

}  // namespace detail
/** @endcond */

}  // namespace bnio::ssl

#endif  // BNIO_SSL_DETAIL_BUFFERS_H_
