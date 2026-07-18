#if defined(BNIO_HEADER_TEST_BUFFER)
#include <bnio/buffer.h>
#elif defined(BNIO_HEADER_TEST_IO_CONTEXT)
#include <bnio/io_context.h>
#elif defined(BNIO_HEADER_TEST_IO_CONTEXT_CPO)
#include <bnio/io_context_cpo.h>
#elif defined(BNIO_HEADER_TEST_IP)
#include <bnio/ip.h>
#elif defined(BNIO_HEADER_TEST_SSL)
#include <bnio/ssl.h>
#elif defined(BNIO_HEADER_TEST_TCP)
#include <bnio/tcp.h>
#elif defined(BNIO_HEADER_TEST_UDP)
#include <bnio/udp.h>
#else
#error "missing header self-contained test definition"
#endif

#include <gtest/gtest.h>

TEST(HeaderSelfContainedTest, compiles) { SUCCEED(); }
