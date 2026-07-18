#if defined(BNIO_HEADER_TEST_ASYNC_IO)
#include <bnio/async_io.h>
#elif defined(BNIO_HEADER_TEST_ADDRESS)
#include <bnio/async_io/address.h>
#elif defined(BNIO_HEADER_TEST_BUFFER_VIEW)
#include <bnio/async_io/buffer_view.h>
#elif defined(BNIO_HEADER_TEST_DESCRIPTOR_VIEW)
#include <bnio/async_io/descriptor_view.h>
#elif defined(BNIO_HEADER_TEST_DNS)
#include <bnio/async_io/dns.h>
#elif defined(BNIO_HEADER_TEST_IP_ADDRESS)
#include <bnio/async_io/ip/address.h>
#elif defined(BNIO_HEADER_TEST_IP_ENDPOINT)
#include <bnio/async_io/ip/endpoint.h>
#elif defined(BNIO_HEADER_TEST_IP_TCP)
#include <bnio/async_io/ip/tcp.h>
#elif defined(BNIO_HEADER_TEST_IP_UDP)
#include <bnio/async_io/ip/udp.h>
#elif defined(BNIO_HEADER_TEST_LINUX_IO_URING_CONTEXT)
#include <bnio/async_io/linux/io_uring_context.h>
#elif defined(BNIO_HEADER_TEST_LINUX_SOCKET_ADDRESS)
#include <bnio/async_io/linux/socket_address.h>
#elif defined(BNIO_HEADER_TEST_BSD_KQUEUE_CONTEXT)
#include <bnio/async_io/bsd/kqueue_context.h>
#elif defined(BNIO_HEADER_TEST_BSD_KQUEUE_HELPER)
#include <bnio/async_io/bsd/kqueue_helper.h>
#elif defined(BNIO_HEADER_TEST_BSD_SOCKET_ADDRESS)
#include <bnio/async_io/bsd/socket_address.h>
#elif defined(BNIO_HEADER_TEST_SOCKET_VIEW)
#include <bnio/async_io/socket_view.h>
#elif defined(BNIO_HEADER_TEST_TCP_ENDPOINT)
#include <bnio/async_io/tcp_endpoint.h>
#elif defined(BNIO_HEADER_TEST_TIME)
#include <bnio/async_io/time.h>
#else
#error "missing header self-contained test definition"
#endif

#include <gtest/gtest.h>

TEST(HeaderSelfContainedTest, compiles) { SUCCEED(); }
