#include <bupp/base/bsd/event.h>

namespace bupp::base {

event::event() noexcept = default;

event::event(uintptr_t ident, int16_t filter, uint16_t flags, uint32_t fflags,
             intptr_t data, void* udata) noexcept {
  set(ident, filter, flags, fflags, data, udata);
}

event::event(const struct kevent& raw_event) noexcept : event_(raw_event) {}

void event::set(uintptr_t ident, int16_t filter, uint16_t flags,
                uint32_t fflags, intptr_t data, void* udata) noexcept {
  EV_SET(&event_, ident, filter, flags, fflags, data, udata);
}

struct kevent* event::raw() noexcept { return &event_; }

const struct kevent* event::raw() const noexcept { return &event_; }

uintptr_t event::ident() const noexcept { return event_.ident; }

int16_t event::filter() const noexcept { return event_.filter; }

uint16_t event::flags() const noexcept { return event_.flags; }

uint32_t event::fflags() const noexcept { return event_.fflags; }

intptr_t event::data() const noexcept { return event_.data; }

void* event::udata() const noexcept { return event_.udata; }

void event::set_ident(uintptr_t ident) noexcept { event_.ident = ident; }

void event::set_filter(int16_t filter) noexcept { event_.filter = filter; }

void event::set_flags(uint16_t flags) noexcept { event_.flags = flags; }

void event::set_fflags(uint32_t fflags) noexcept { event_.fflags = fflags; }

void event::set_data(intptr_t data) noexcept { event_.data = data; }

void event::set_udata(void* udata) noexcept { event_.udata = udata; }

bool event::has_error() const noexcept {
  return (event_.flags & EV_ERROR) != 0;
}

bool event::has_eof() const noexcept { return (event_.flags & EV_EOF) != 0; }

}  // namespace bupp::base
