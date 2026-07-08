#include <bupp/base/linux/completion_queue_entry.h>
#include <liburing.h>

#include <cassert>
#include <cstdint>

int main() {
  int marker = 0;
  io_uring_cqe raw_cqe{};
  raw_cqe.user_data = reinterpret_cast<std::uintptr_t>(&marker);
  raw_cqe.res = 7;
  raw_cqe.flags = 3;

  bupp::base::completion_queue_entry cqe(&raw_cqe);

  assert(cqe.raw() == &raw_cqe);
  assert(cqe.res() == 7);
  assert(cqe.flags() == raw_cqe.flags);
  assert(cqe.get_data() == &marker);
  assert(cqe.get_data64() == raw_cqe.user_data);

  return 0;
}
