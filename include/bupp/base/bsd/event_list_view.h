#pragma once
#ifndef BUPP_BASE_BSD_EVENT_LIST_VIEW_H_
#define BUPP_BASE_BSD_EVENT_LIST_VIEW_H_

#include <bupp/base/bsd/event.h>
#include <bupp/export.h>

#include <cstddef>

namespace bupp::base {

/**
 * Non-owning view over a caller-owned contiguous event array.
 */
class BUPP_EXPORT event_list_view {
 public:
  event_list_view() noexcept;
  event_list_view(event* events, std::size_t size) noexcept;

  [[nodiscard]] event* data() noexcept;
  [[nodiscard]] const event* data() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;

  [[nodiscard]] event& operator[](std::size_t index) noexcept;
  [[nodiscard]] const event& operator[](std::size_t index) const noexcept;

 private:
  event* events_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace bupp::base

#endif  // BUPP_BASE_BSD_EVENT_LIST_VIEW_H_
