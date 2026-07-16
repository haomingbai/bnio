#if defined(BUPP_HEADER_TEST_BASE)
#include <bupp/base.h>
#elif defined(BUPP_HEADER_TEST_EVENT)
#include <bupp/base/bsd/event.h>
#elif defined(BUPP_HEADER_TEST_EVENT_LIST_VIEW)
#include <bupp/base/bsd/event_list_view.h>
#elif defined(BUPP_HEADER_TEST_KQUEUE)
#include <bupp/base/bsd/kqueue.h>
#elif defined(BUPP_HEADER_TEST_BUPP)
#include <bupp/bupp.h>
#else
#error "missing header self-contained test definition"
#endif

#include <gtest/gtest.h>

TEST(HeaderSelfContainedTest, compiles) { SUCCEED(); }
