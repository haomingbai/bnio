#include <bupp/base/linux/ring.h>
#include <bupp/base/linux/submission_queue_entry.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <string_view>

#include "example_support.h"

namespace {

class unlink_on_exit {
 public:
  explicit unlink_on_exit(char* path) noexcept : path_(path) {}

  ~unlink_on_exit() noexcept {
    if (path_ != nullptr) {
      static_cast<void>(::unlink(path_));
    }
  }

  unlink_on_exit(const unlink_on_exit&) = delete;
  unlink_on_exit& operator=(const unlink_on_exit&) = delete;

 private:
  char* path_ = nullptr;
};

}  // namespace

int main() {
  bupp::base::ring ring;
  switch (bupp::examples::base::init_ring(ring, 16, "file_io")) {
    case bupp::examples::base::ring_init_result::ready:
      break;
    case bupp::examples::base::ring_init_result::unavailable:
      return 0;
    case bupp::examples::base::ring_init_result::failed:
      return 1;
  }

  std::array<char, 64> path{};
  constexpr char k_template[] = "/tmp/bupp-file-io-XXXXXX";
  std::copy(std::begin(k_template), std::end(k_template), path.begin());

  bupp::examples::base::unique_fd file(::mkstemp(path.data()));
  if (!file.is_open()) {
    std::cerr << "file_io: mkstemp failed\n";
    return 1;
  }
  unlink_on_exit cleanup(path.data());

  constexpr std::string_view k_payload = "bupp file I/O through io_uring\n";
  constexpr std::uint64_t k_write_user_data = 1;
  constexpr std::uint64_t k_fsync_user_data = 2;
  constexpr std::uint64_t k_read_user_data = 3;
  constexpr std::uint64_t k_close_user_data = 4;

  bupp::base::submission_queue_entry sqe =
      bupp::examples::base::get_sqe_or_log(ring, "file_io write");
  if (sqe.raw() == nullptr) {
    return 1;
  }
  sqe.prep_write(file.get(), k_payload.data(),
                 static_cast<unsigned>(k_payload.size()), 0);
  sqe.set_data64(k_write_user_data);

  const int write_result = bupp::examples::base::submit_and_wait_one(
      ring, k_write_user_data, "file_io write");
  if (write_result != static_cast<int>(k_payload.size())) {
    std::cerr << "file_io: write result=" << write_result << '\n';
    return 1;
  }

  sqe = bupp::examples::base::get_sqe_or_log(ring, "file_io fsync");
  if (sqe.raw() == nullptr) {
    return 1;
  }
  sqe.prep_fsync(file.get(), 0);
  sqe.set_data64(k_fsync_user_data);

  const int fsync_result = bupp::examples::base::submit_and_wait_one(
      ring, k_fsync_user_data, "file_io fsync");
  if (fsync_result < 0) {
    std::cerr << "file_io: fsync result=" << fsync_result << '\n';
    return 1;
  }

  std::array<char, 128> read_buffer{};
  sqe = bupp::examples::base::get_sqe_or_log(ring, "file_io read");
  if (sqe.raw() == nullptr) {
    return 1;
  }
  sqe.prep_read(file.get(), read_buffer.data(),
                static_cast<unsigned>(read_buffer.size()), 0);
  sqe.set_data64(k_read_user_data);

  const int read_result = bupp::examples::base::submit_and_wait_one(
      ring, k_read_user_data, "file_io read");
  if (read_result != static_cast<int>(k_payload.size())) {
    std::cerr << "file_io: read result=" << read_result << '\n';
    return 1;
  }

  const std::string_view read_payload(read_buffer.data(),
                                      static_cast<std::size_t>(read_result));
  if (read_payload != k_payload) {
    std::cerr << "file_io: read payload mismatch\n";
    return 1;
  }

  sqe = bupp::examples::base::get_sqe_or_log(ring, "file_io close");
  if (sqe.raw() == nullptr) {
    return 1;
  }
  sqe.prep_close(file.get());
  sqe.set_data64(k_close_user_data);
  static_cast<void>(file.release());

  const int close_result = bupp::examples::base::submit_and_wait_one(
      ring, k_close_user_data, "file_io close");
  if (close_result < 0) {
    std::cerr << "file_io: close result=" << close_result << '\n';
    return 1;
  }

  std::cout << "file_io wrote and read " << read_result << " bytes\n";
  return 0;
}
