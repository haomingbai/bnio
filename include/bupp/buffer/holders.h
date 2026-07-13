#pragma once
#ifndef BUPP_BUFFER_HOLDERS_H_
#define BUPP_BUFFER_HOLDERS_H_

#include <bupp/buffer/basic.h>
#include <bupp/buffer/dynamic_byte_vector.h>
#include <bupp/buffer/dynamic_string.h>

#include <concepts>
#include <cstddef>
#include <type_traits>

namespace bupp {

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
    auto data = buffer(value);
    if constexpr (std::same_as<decltype(data), mutable_buffer>) {
      return const_buffer_holder(const_buffer(data.data(), data.size()));
    } else {
      return const_buffer_holder(data);
    }
  }
}

}  // namespace detail
/** @endcond */

}  // namespace bupp

#endif  // BUPP_BUFFER_HOLDERS_H_
