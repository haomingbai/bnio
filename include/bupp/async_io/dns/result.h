#pragma once
#ifndef BUPP_ASYNC_IO_DNS_RESULT_H_
#define BUPP_ASYNC_IO_DNS_RESULT_H_

#include <bupp/async_io/ip/endpoint.h>

#include <array>
#include <cstddef>

namespace bupp::async_io {

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

 private:
  endpoint_type* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace bupp::async_io

#endif  // BUPP_ASYNC_IO_DNS_RESULT_H_
