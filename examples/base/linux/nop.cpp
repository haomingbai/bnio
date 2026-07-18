#include <bnio/base/linux/completion_queue_entry.h>
#include <bnio/base/linux/ring.h>
#include <bnio/base/linux/submission_queue_entry.h>

#include <cstdint>
#include <iostream>

#include "example_support.h"

int main() {
  bnio::base::ring ring;
  switch (bnio::examples::base::init_ring(ring, 8, "nop")) {
    case bnio::examples::base::ring_init_result::ready:
      break;
    case bnio::examples::base::ring_init_result::unavailable:
      return 0;
    case bnio::examples::base::ring_init_result::failed:
      return 1;
  }

  bnio::base::submission_queue_entry sqe =
      bnio::examples::base::get_sqe_or_log(ring, "nop");
  if (sqe.raw() == nullptr) {
    return 1;
  }

  constexpr std::uint64_t k_user_data = 42;
  sqe.prep_nop();
  sqe.set_data64(k_user_data);

  const int submit_result = ring.submit();
  if (submit_result < 0) {
    std::cerr << "io_uring_submit failed: " << submit_result << '\n';
    return 1;
  }

  bnio::base::completion_queue_entry cqe;
  const int wait_result = ring.wait_cqe(cqe);
  if (wait_result < 0) {
    std::cerr << "io_uring_wait_cqe failed: " << wait_result << '\n';
    return 1;
  }

  const int cqe_result = cqe.res();
  const std::uint64_t user_data = cqe.get_data64();

  std::cout << "nop res=" << cqe_result << " user_data=" << user_data << '\n';

  ring.cqe_seen(cqe);
  return cqe_result == 0 && user_data == k_user_data ? 0 : 1;
}
