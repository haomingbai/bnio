#if defined(BUPP_HEADER_TEST_BUFFER)
#include <bupp/buffer.h>
#elif defined(BUPP_HEADER_TEST_IO_CONTEXT)
#include <bupp/io_context.h>
#elif defined(BUPP_HEADER_TEST_IO_CONTEXT_CPO)
#include <bupp/io_context_cpo.h>
#elif defined(BUPP_HEADER_TEST_IP)
#include <bupp/ip.h>
#elif defined(BUPP_HEADER_TEST_SSL)
#include <bupp/ssl.h>
#elif defined(BUPP_HEADER_TEST_TCP)
#include <bupp/tcp.h>
#elif defined(BUPP_HEADER_TEST_UDP)
#include <bupp/udp.h>
#else
#error "missing header self-contained test definition"
#endif

int main() { return 0; }
