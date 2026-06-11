#include <bupp/base/completion_queue_entry.h>
#include <bupp/base/ring.h>
#include <bupp/base/submission_queue_entry.h>
#include <liburing.h>
#include <sys/socket.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

#include "example_support.h"

int main() {
  bupp::base::ring ring;
  switch (bupp::examples::base::init_ring(ring, 16, "provided_buffers")) {
    case bupp::examples::base::ring_init_result::ready:
      break;
    case bupp::examples::base::ring_init_result::unavailable:
      return 0;
    case bupp::examples::base::ring_init_result::failed:
      return 1;
  }

  int sockets[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
    std::cerr << "provided_buffers: socketpair failed\n";
    return 1;
  }
  bupp::examples::base::unique_fd receiver(sockets[0]);
  bupp::examples::base::unique_fd sender(sockets[1]);

  constexpr int k_buffer_group = 7;
  constexpr int k_buffer_id = 3;
  constexpr std::uint64_t k_provide_user_data = 1;
  constexpr std::uint64_t k_recv_user_data = 2;
  std::array<char, 128> provided_buffer{};

  bupp::base::submission_queue_entry sqe =
      bupp::examples::base::get_sqe_or_log(ring, "provided_buffers provide");
  if (sqe.raw() == nullptr) {
    return 1;
  }
  sqe.prep_provide_buffers(provided_buffer.data(),
                           static_cast<int>(provided_buffer.size()), 1,
                           k_buffer_group, k_buffer_id);
  sqe.set_data64(k_provide_user_data);

  const int provide_result = bupp::examples::base::submit_and_wait_one(
      ring, k_provide_user_data, "provided_buffers provide");
  if (provide_result < 0) {
    if (bupp::examples::base::is_feature_unavailable_result(provide_result)) {
      std::cerr << "provided_buffers: feature is not available: "
                << provide_result << '\n';
      return 0;
    }
    std::cerr << "provided_buffers: provide result=" << provide_result << '\n';
    return 1;
  }

  constexpr std::string_view k_payload = "selected from a provided buffer";
  if (::send(sender.get(), k_payload.data(), k_payload.size(), 0) !=
      static_cast<ssize_t>(k_payload.size())) {
    std::cerr << "provided_buffers: send failed\n";
    return 1;
  }

  sqe = bupp::examples::base::get_sqe_or_log(ring, "provided_buffers recv");
  if (sqe.raw() == nullptr) {
    return 1;
  }
  sqe.prep_recv(receiver.get(), nullptr, provided_buffer.size(), 0);
  sqe.set_flags(static_cast<unsigned>(IOSQE_BUFFER_SELECT));
  sqe.set_buf_group(k_buffer_group);
  sqe.set_data64(k_recv_user_data);

  const int submit_result = ring.submit();
  if (submit_result < 0) {
    std::cerr << "provided_buffers: recv submit failed: " << submit_result
              << '\n';
    return 1;
  }

  bupp::base::completion_queue_entry cqe;
  const int wait_result = ring.wait_cqe(cqe);
  if (wait_result < 0) {
    std::cerr << "provided_buffers: recv wait failed: " << wait_result << '\n';
    return 1;
  }

  const int recv_result = cqe.res();
  const std::uint64_t user_data = cqe.get_data64();
  const bool has_buffer = cqe.has_buffer();
  const unsigned buffer_id = cqe.buffer_id();
  ring.cqe_seen(cqe);

  if (recv_result < 0) {
    if (bupp::examples::base::is_feature_unavailable_result(recv_result)) {
      std::cerr << "provided_buffers: receive selection is not available: "
                << recv_result << '\n';
      return 0;
    }
    std::cerr << "provided_buffers: recv result=" << recv_result << '\n';
    return 1;
  }

  const std::string_view selected_payload(
      provided_buffer.data(), static_cast<std::size_t>(recv_result));
  if (user_data != k_recv_user_data || !has_buffer ||
      buffer_id != static_cast<unsigned>(k_buffer_id) ||
      selected_payload != k_payload) {
    std::cerr << "provided_buffers: unexpected selected buffer completion\n";
    return 1;
  }

  std::cout << "provided_buffers received \"" << selected_payload
            << "\" in buffer_id=" << buffer_id << '\n';
  return 0;
}
