/**
 * @file event_list_view.cpp
 * @brief kevent array view implementation.
 */

#include <bnio/base/bsd/event_list_view.h>
namespace bnio::base {

event_list_view::event_list_view() noexcept = default;

event_list_view::event_list_view(event* events, std::size_t size) noexcept
    : events_(events), size_(size) {}

event* event_list_view::data() noexcept { return events_; }

const event* event_list_view::data() const noexcept { return events_; }

std::size_t event_list_view::size() const noexcept { return size_; }

bool event_list_view::empty() const noexcept { return size_ == 0; }

event& event_list_view::operator[](std::size_t index) noexcept {
  return events_[index];
}

const event& event_list_view::operator[](std::size_t index) const noexcept {
  return events_[index];
}

}  // namespace bnio::base
