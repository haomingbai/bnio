/**
 * @file event_list_view.h
 * @brief Non-owning view of a kevent array.
 */

#pragma once
#ifndef BNIO_BASE_BSD_EVENT_LIST_VIEW_H_
#define BNIO_BASE_BSD_EVENT_LIST_VIEW_H_

#include <bnio/base/bsd/event.h>
#include <bnio/export.h>

#include <cstddef>

namespace bnio::base {

/**
 * Non-owning view over a caller-owned contiguous event array.
 */
class BNIO_EXPORT event_list_view {
 public:
  /**
   * Creates an empty view.
   */
  event_list_view() noexcept;

  /**
   * Creates a view over a contiguous event array.
   */
  event_list_view(event* events, std::size_t size) noexcept;

  /**
   * Returns the first event in the viewed array.
   */
  [[nodiscard]] event* data() noexcept;

  /**
   * Returns the first event in the viewed array.
   */
  [[nodiscard]] const event* data() const noexcept;

  /**
   * Returns the number of events in the view.
   */
  [[nodiscard]] std::size_t size() const noexcept;

  /**
   * Returns whether the view contains no events.
   */
  [[nodiscard]] bool empty() const noexcept;

  /**
   * Returns the event at the requested index.
   */
  [[nodiscard]] event& operator[](std::size_t index) noexcept;

  /**
   * Returns the event at the requested index.
   */
  [[nodiscard]] const event& operator[](std::size_t index) const noexcept;

 private:
  event* events_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace bnio::base

#endif  // BNIO_BASE_BSD_EVENT_LIST_VIEW_H_
