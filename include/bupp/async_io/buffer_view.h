#pragma once
#ifndef BUPP_ASYNC_IO_BUFFER_VIEW_H_
#define BUPP_ASYNC_IO_BUFFER_VIEW_H_

#include <cstddef>

namespace bupp::async_io {

/**
 * Non-owning view over a mutable byte buffer.
 *
 * The type is an aggregate and relies on the compiler-generated copy, move, and
 * destruction operations. Copying a buffer_view copies only the pointer and
 * size; it never owns or extends the lifetime of the referenced bytes.
 */
struct buffer_view {
  /**
   * Pointer to the first byte in the buffer.
   */
  void* data = nullptr;

  /**
   * Number of bytes available through data.
   */
  std::size_t size = 0;
};

}  // namespace bupp::async_io

#endif  // BUPP_ASYNC_IO_BUFFER_VIEW_H_
