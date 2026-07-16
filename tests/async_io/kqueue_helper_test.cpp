#include <bupp/async_io/bsd/kqueue_helper.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

namespace {

using bupp::async_io::bsd_native::kqueue_helper;
using bupp::async_io::bsd_native::kqueue_task;

TEST(KqueueHelperTest, nop_has_no_native_event) {
  kqueue_helper helper;
  helper.prep_nop();
  EXPECT_TRUE(helper.error() == 0);
  EXPECT_TRUE(helper.task() == kqueue_task::nop);
  EXPECT_TRUE(helper.event_count() == 0);
}

TEST(KqueueHelperTest, read_and_write_fill_native_event) {
  kqueue_helper helper;
  helper.prep_read(17);
  EXPECT_TRUE(helper.error() == 0);
  EXPECT_TRUE(helper.task() == kqueue_task::read);
  EXPECT_TRUE(helper.descriptor() == 17);
  EXPECT_TRUE(helper.event_count() == 1);
  EXPECT_TRUE(helper.event().ident() == 17);
  EXPECT_TRUE(helper.event().filter() == EVFILT_READ);
  EXPECT_TRUE((helper.event().flags() & EV_ADD) != 0);
  EXPECT_TRUE((helper.event().flags() & EV_ONESHOT) != 0);

  helper.prep_write(23);
  EXPECT_TRUE(helper.task() == kqueue_task::write);
  EXPECT_TRUE(helper.event_count() == 1);
  EXPECT_TRUE(helper.event().ident() == 23);
  EXPECT_TRUE(helper.event().filter() == EVFILT_WRITE);
}

TEST(KqueueHelperTest, poll_can_prepare_both_filters_and_udata) {
  kqueue_helper helper;
  helper.prep_poll_add(31, static_cast<unsigned>(POLLIN | POLLOUT));
  EXPECT_TRUE(helper.error() == 0);
  EXPECT_TRUE(helper.task() == kqueue_task::poll);
  EXPECT_TRUE(helper.event_count() == 2);
  EXPECT_TRUE(helper.event(0).filter() == EVFILT_READ);
  EXPECT_TRUE(helper.event(1).filter() == EVFILT_WRITE);

  std::uintptr_t value = 42;
  helper.set_udata(&value);
  EXPECT_TRUE(helper.event(0).udata() == &value);
  EXPECT_TRUE(helper.event(1).udata() == &value);
}

TEST(KqueueHelperTest, poll_rejects_an_empty_readiness_mask) {
  kqueue_helper helper;
  helper.prep_poll_add(7, 0);
  EXPECT_TRUE(helper.error() < 0);
  EXPECT_TRUE(helper.event_count() == 0);
}

}  // namespace
