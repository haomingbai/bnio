#include <bupp/base/completion_queue_entry.h>
#include <bupp/base/ring.h>
#include <bupp/base/submission_queue_entry.h>
#include <liburing.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iostream>

#include "example_support.h"

int main() {
  bupp::base::ring ring;
  switch (bupp::examples::base::init_ring(ring, 8, "timeout")) {
    case bupp::examples::base::ring_init_result::ready:
      break;
    case bupp::examples::base::ring_init_result::unavailable:
      return 0;
    case bupp::examples::base::ring_init_result::failed:
      return 1;
  }

  bupp::base::submission_queue_entry sqe =
      bupp::examples::base::get_sqe_or_log(ring, "timeout");
  if (sqe.raw() == nullptr) {
    return 1;
  }

  __kernel_timespec timeout{.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
  constexpr std::uint64_t k_user_data = 1;

  sqe.prep_timeout(&timeout, 0, 0);
  sqe.set_data64(k_user_data);

  const auto started_at = std::chrono::steady_clock::now();
  const int result =
      bupp::examples::base::submit_and_wait_one(ring, k_user_data, "timeout");
  const auto finished_at = std::chrono::steady_clock::now();

  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      finished_at - started_at);

  std::cout << "timeout res=" << result << " elapsed_ms=" << elapsed_ms.count()
            << '\n';
  return result == -ETIME ? 0 : 1;
}
