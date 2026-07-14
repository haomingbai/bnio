#if defined(BUPP_HEADER_TEST_ASYNC_IO)
#include <bupp/async_io.h>
#elif defined(BUPP_HEADER_TEST_ADDRESS)
#include <bupp/async_io/address.h>
#elif defined(BUPP_HEADER_TEST_BUFFER_VIEW)
#include <bupp/async_io/buffer_view.h>
#elif defined(BUPP_HEADER_TEST_DESCRIPTOR_VIEW)
#include <bupp/async_io/descriptor_view.h>
#elif defined(BUPP_HEADER_TEST_DNS)
#include <bupp/async_io/dns.h>
#elif defined(BUPP_HEADER_TEST_IP_ADDRESS)
#include <bupp/async_io/ip/address.h>
#elif defined(BUPP_HEADER_TEST_IP_ENDPOINT)
#include <bupp/async_io/ip/endpoint.h>
#elif defined(BUPP_HEADER_TEST_IP_TCP)
#include <bupp/async_io/ip/tcp.h>
#elif defined(BUPP_HEADER_TEST_IP_UDP)
#include <bupp/async_io/ip/udp.h>
#elif defined(BUPP_HEADER_TEST_LINUX_IO_URING_CONTEXT)
#include <bupp/async_io/linux/io_uring_context.h>
#elif defined(BUPP_HEADER_TEST_LINUX_SOCKET_ADDRESS)
#include <bupp/async_io/linux/socket_address.h>
#elif defined(BUPP_HEADER_TEST_BSD_KQUEUE_CONTEXT)
#include <bupp/async_io/bsd/kqueue_context.h>
#elif defined(BUPP_HEADER_TEST_BSD_KQUEUE_HELPER)
#include <bupp/async_io/bsd/kqueue_helper.h>
#elif defined(BUPP_HEADER_TEST_BSD_SOCKET_ADDRESS)
#include <bupp/async_io/bsd/socket_address.h>
#elif defined(BUPP_HEADER_TEST_SOCKET_VIEW)
#include <bupp/async_io/socket_view.h>
#elif defined(BUPP_HEADER_TEST_TCP_ENDPOINT)
#include <bupp/async_io/tcp_endpoint.h>
#elif defined(BUPP_HEADER_TEST_TIME)
#include <bupp/async_io/time.h>
#else
#error "missing header self-contained test definition"
#endif

int main() { return 0; }
