#if defined(BNIO_HEADER_TEST_BASE)
#include <bnio/base.h>
#elif defined(BNIO_HEADER_TEST_EVENT)
#include <bnio/base/bsd/event.h>
#elif defined(BNIO_HEADER_TEST_EVENT_LIST_VIEW)
#include <bnio/base/bsd/event_list_view.h>
#elif defined(BNIO_HEADER_TEST_KQUEUE)
#include <bnio/base/bsd/kqueue.h>
#elif defined(BNIO_HEADER_TEST_BNIO)
#include <bnio/bnio.h>
#else
#error "missing header self-contained test definition"
#endif

#include <gtest/gtest.h>

TEST(HeaderSelfContainedTest, compiles) { SUCCEED(); }
