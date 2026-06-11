#include <bupp/base/linux/completion_queue_entry.h>
#include <bupp/base/linux/ring.h>
#include <bupp/base/linux/submission_queue_entry.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cstdint>
#include <iostream>

#include "example_support.h"

int main() {
  bupp::base::ring ring;
  switch (bupp::examples::base::init_ring(ring, 8, "poll")) {
    case bupp::examples::base::ring_init_result::ready:
      break;
    case bupp::examples::base::ring_init_result::unavailable:
      return 0;
    case bupp::examples::base::ring_init_result::failed:
      return 1;
  }

  int pipe_fds[2] = {-1, -1};
  if (::pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) != 0) {
    std::cerr << "poll: pipe2 failed\n";
    return 1;
  }
  bupp::examples::base::unique_fd read_end(pipe_fds[0]);
  bupp::examples::base::unique_fd write_end(pipe_fds[1]);

  bupp::base::submission_queue_entry sqe =
      bupp::examples::base::get_sqe_or_log(ring, "poll");
  if (sqe.raw() == nullptr) {
    return 1;
  }

  constexpr std::uint64_t k_user_data = 1;
  sqe.prep_poll_add(read_end.get(), static_cast<unsigned>(POLLIN));
  sqe.set_data64(k_user_data);

  const int submit_result = ring.submit();
  if (submit_result < 0) {
    std::cerr << "poll: submit failed: " << submit_result << '\n';
    return 1;
  }

  constexpr char k_byte = 'x';
  if (::write(write_end.get(), &k_byte, 1) != 1) {
    std::cerr << "poll: write failed\n";
    return 1;
  }

  bupp::base::completion_queue_entry cqe;
  const int wait_result = ring.wait_cqe(cqe);
  if (wait_result < 0) {
    std::cerr << "poll: wait failed: " << wait_result << '\n';
    return 1;
  }

  const int result = cqe.res();
  const std::uint64_t user_data = cqe.get_data64();
  ring.cqe_seen(cqe);

  if (user_data != k_user_data || result < 0 ||
      (static_cast<unsigned>(result) & static_cast<unsigned>(POLLIN)) == 0) {
    std::cerr << "poll: unexpected completion result=" << result << '\n';
    return 1;
  }

  std::cout << "poll observed readable pipe event\n";
  return 0;
}
