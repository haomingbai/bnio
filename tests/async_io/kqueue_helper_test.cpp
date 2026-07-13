#include <bupp/async_io/bsd/kqueue_helper.h>

#include <cassert>
#include <cstdint>

namespace {

using bupp::async_io::bsd_native::kqueue_helper;
using bupp::async_io::bsd_native::kqueue_task;

void test_nop_has_no_native_event() {
  kqueue_helper helper;
  helper.prep_nop();
  assert(helper.error() == 0);
  assert(helper.task() == kqueue_task::nop);
  assert(helper.event_count() == 0);
}

void test_read_and_write_fill_native_event() {
  kqueue_helper helper;
  helper.prep_read(17);
  assert(helper.error() == 0);
  assert(helper.task() == kqueue_task::read);
  assert(helper.descriptor() == 17);
  assert(helper.event_count() == 1);
  assert(helper.event().ident() == 17);
  assert(helper.event().filter() == EVFILT_READ);
  assert((helper.event().flags() & EV_ADD) != 0);
  assert((helper.event().flags() & EV_ONESHOT) != 0);

  helper.prep_write(23);
  assert(helper.task() == kqueue_task::write);
  assert(helper.event_count() == 1);
  assert(helper.event().ident() == 23);
  assert(helper.event().filter() == EVFILT_WRITE);
}

void test_poll_can_prepare_both_filters_and_udata() {
  kqueue_helper helper;
  helper.prep_poll_add(31, static_cast<unsigned>(POLLIN | POLLOUT));
  assert(helper.error() == 0);
  assert(helper.task() == kqueue_task::poll);
  assert(helper.event_count() == 2);
  assert(helper.event(0).filter() == EVFILT_READ);
  assert(helper.event(1).filter() == EVFILT_WRITE);

  std::uintptr_t value = 42;
  helper.set_udata(&value);
  assert(helper.event(0).udata() == &value);
  assert(helper.event(1).udata() == &value);
}

void test_poll_rejects_an_empty_readiness_mask() {
  kqueue_helper helper;
  helper.prep_poll_add(7, 0);
  assert(helper.error() < 0);
  assert(helper.event_count() == 0);
}

}  // namespace

int main() {
  test_nop_has_no_native_event();
  test_read_and_write_fill_native_event();
  test_poll_can_prepare_both_filters_and_udata();
  test_poll_rejects_an_empty_readiness_mask();
  return 0;
}
