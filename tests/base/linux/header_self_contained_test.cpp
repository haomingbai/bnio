#if defined(BUPP_HEADER_TEST_BASE)
#include <bupp/base.h>
#elif defined(BUPP_HEADER_TEST_COMPLETION_QUEUE_ENTRY)
#include <bupp/base/linux/completion_queue_entry.h>
#elif defined(BUPP_HEADER_TEST_PARAMS)
#include <bupp/base/linux/params.h>
#elif defined(BUPP_HEADER_TEST_PROBE)
#include <bupp/base/linux/probe.h>
#elif defined(BUPP_HEADER_TEST_RING)
#include <bupp/base/linux/ring.h>
#elif defined(BUPP_HEADER_TEST_SUBMISSION_QUEUE_ENTRY)
#include <bupp/base/linux/submission_queue_entry.h>
#elif defined(BUPP_HEADER_TEST_BUPP)
#include <bupp/bupp.h>
#else
#error "missing header self-contained test definition"
#endif

int main() { return 0; }
