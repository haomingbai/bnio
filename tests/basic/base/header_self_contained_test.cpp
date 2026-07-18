#if defined(BNIO_HEADER_TEST_BASE)
#include <bnio/base.h>
#elif defined(BNIO_HEADER_TEST_COMPLETION_QUEUE_ENTRY)
#include <bnio/base/linux/completion_queue_entry.h>
#elif defined(BNIO_HEADER_TEST_PARAMS)
#include <bnio/base/linux/params.h>
#elif defined(BNIO_HEADER_TEST_PROBE)
#include <bnio/base/linux/probe.h>
#elif defined(BNIO_HEADER_TEST_RING)
#include <bnio/base/linux/ring.h>
#elif defined(BNIO_HEADER_TEST_SUBMISSION_QUEUE_ENTRY)
#include <bnio/base/linux/submission_queue_entry.h>
#elif defined(BNIO_HEADER_TEST_BNIO)
#include <bnio/bnio.h>
#else
#error "missing header self-contained test definition"
#endif

#include <gtest/gtest.h>

TEST(HeaderSelfContainedTest, compiles) { SUCCEED(); }
