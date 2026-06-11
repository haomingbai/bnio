#include <bupp/base/completion_queue_entry.h>
#include <liburing.h>

#include <cassert>
#include <cstdint>

int main() {
  int marker = 0;
  io_uring_cqe raw_cqe{};
  raw_cqe.user_data = reinterpret_cast<std::uintptr_t>(&marker);
  raw_cqe.res = 7;
  raw_cqe.flags = IORING_CQE_F_MORE | IORING_CQE_F_BUFFER |
                  (13U << IORING_CQE_BUFFER_SHIFT);

  bupp::base::completion_queue_entry cqe(&raw_cqe);

  assert(cqe.raw() == &raw_cqe);
  assert(cqe.res() == 7);
  assert(cqe.flags() == raw_cqe.flags);
  assert(cqe.get_data() == &marker);
  assert(cqe.get_data64() == raw_cqe.user_data);
  assert(cqe.has_more());
  assert(cqe.has_buffer());
  assert(cqe.buffer_id() == 13);

  return 0;
}
