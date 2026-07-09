#pragma once
#ifndef BUPP_BASE_BSD_EVENT_H_
#define BUPP_BASE_BSD_EVENT_H_

#include <bupp/export.h>
#include <sys/event.h>

#include <cstdint>
#include <type_traits>

namespace bupp::base {

/**
 * Value wrapper for a native kqueue event.
 *
 * @see kevent
 */
class BUPP_EXPORT event {
 public:
  /**
   * Creates a zero-initialized event.
   */
  event() noexcept;

  /**
   * Creates an event from the native kevent fields.
   *
   * @see EV_SET
   */
  event(uintptr_t ident, int16_t filter, uint16_t flags, uint32_t fflags,
        intptr_t data, void* udata) noexcept;

  /**
   * Wraps an existing native kevent value.
   */
  explicit event(const struct kevent& raw_event) noexcept;

  /**
   * Sets all native kevent fields.
   *
   * @see EV_SET
   */
  void set(uintptr_t ident, int16_t filter, uint16_t flags, uint32_t fflags,
           intptr_t data, void* udata) noexcept;

  /**
   * Returns the wrapped native event.
   */
  [[nodiscard]] struct kevent* raw() noexcept;

  /**
   * Returns the wrapped native event.
   */
  [[nodiscard]] const struct kevent* raw() const noexcept;

  [[nodiscard]] uintptr_t ident() const noexcept;
  [[nodiscard]] int16_t filter() const noexcept;
  [[nodiscard]] uint16_t flags() const noexcept;
  [[nodiscard]] uint32_t fflags() const noexcept;
  [[nodiscard]] intptr_t data() const noexcept;
  [[nodiscard]] void* udata() const noexcept;

  void set_ident(uintptr_t ident) noexcept;
  void set_filter(int16_t filter) noexcept;
  void set_flags(uint16_t flags) noexcept;
  void set_fflags(uint32_t fflags) noexcept;
  void set_data(intptr_t data) noexcept;
  void set_udata(void* udata) noexcept;

  [[nodiscard]] bool has_error() const noexcept;
  [[nodiscard]] bool has_eof() const noexcept;

 private:
  struct kevent event_ {};
};

static_assert(std::is_standard_layout_v<event>);
static_assert(sizeof(event) == sizeof(struct kevent));
static_assert(alignof(event) == alignof(struct kevent));

}  // namespace bupp::base

#endif  // BUPP_BASE_BSD_EVENT_H_
