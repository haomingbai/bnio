/**
 * @file result.h
 * @brief DNS resolution result.
 */

#pragma once
#ifndef BNIO_ASYNC_IO_DNS_RESULT_H_
#define BNIO_ASYNC_IO_DNS_RESULT_H_

#include <bnio/async_io/ip/endpoint.h>

#include <array>
#include <cstddef>

namespace bnio::async_io {

/**
 * Non-owning mutable view over caller-owned endpoint storage.
 *
 * The view does not track how many endpoints are written. Resolver operations
 * report that count through set_value(count) or an output count parameter.
 */
class dns_result_view {
 public:
  /**
   * Endpoint type stored in the result view.
   */
  using endpoint_type = ip::endpoint;

  /**
   * Iterator type. The view is a mutable view, so even const iteration yields
   * writable endpoints (consistent with data() and operator[]).
   */
  using iterator = endpoint_type*;

  /**
   * Creates an empty result view.
   */
  constexpr dns_result_view() noexcept = default;

  /**
   * Creates a result view from writable endpoint storage and capacity.
   */
  constexpr dns_result_view(endpoint_type* data, std::size_t size) noexcept
      : data_(data), size_(size) {}

  /**
   * Creates a result view over a std::array.
   */
  template <std::size_t Size>
  constexpr explicit dns_result_view(
      std::array<endpoint_type, Size>& storage) noexcept
      : dns_result_view(storage.data(), storage.size()) {}

  /**
   * Creates a result view over a C array.
   */
  template <std::size_t Size>
  constexpr explicit dns_result_view(endpoint_type (&storage)[Size]) noexcept
      : dns_result_view(storage, Size) {}

  /**
   * Returns the writable endpoint storage.
   */
  [[nodiscard]] constexpr endpoint_type* data() const noexcept { return data_; }

  /**
   * Returns the maximum number of endpoints that may be written.
   */
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }

  /**
   * Returns whether this view is writable, or intentionally empty.
   */
  [[nodiscard]] constexpr bool valid() const noexcept {
    return data_ != nullptr || size_ == 0;
  }

  /**
   * Returns the endpoint at an index.
   */
  [[nodiscard]] constexpr endpoint_type& operator[](
      std::size_t index) const noexcept {
    return data_[index];
  }

  /**
   * Returns an iterator to the first endpoint in the viewed storage.
   * Iteration covers the full capacity reported by size(); callers that only
   * want the resolved subset must pair this with a count from set_value().
   */
  [[nodiscard]] constexpr iterator begin() const noexcept { return data_; }

  /**
   * Returns a past-the-end iterator for the viewed storage.
   */
  [[nodiscard]] constexpr iterator end() const noexcept {
    return data_ + size_;
  }

 private:
  endpoint_type* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace bnio::async_io

#endif  // BNIO_ASYNC_IO_DNS_RESULT_H_
