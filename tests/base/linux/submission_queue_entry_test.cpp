#include <bupp/base/linux/submission_queue_entry.h>
#include <fcntl.h>
#include <liburing.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include <array>
#include <cassert>
#include <cstdint>

namespace {

constexpr std::uint64_t k_user_data = 0x62757070ULL;

bupp::base::submission_queue_entry reset_sqe(io_uring_sqe& raw_sqe) {
  raw_sqe = {};
  io_uring_initialize_sqe(&raw_sqe);
  return bupp::base::submission_queue_entry(&raw_sqe);
}

void test_metadata_helpers(io_uring_sqe& raw_sqe) {
  int marker = 0;
  bupp::base::submission_queue_entry sqe = reset_sqe(raw_sqe);

  sqe.set_data(&marker);
  assert(raw_sqe.user_data == reinterpret_cast<std::uintptr_t>(&marker));

  sqe.set_data64(k_user_data);
  assert(raw_sqe.user_data == k_user_data);

  sqe.set_flags(static_cast<unsigned>(IOSQE_BUFFER_SELECT));
  assert(raw_sqe.flags == IOSQE_BUFFER_SELECT);

  sqe.set_buf_group(7);
  assert(raw_sqe.buf_group == 7);
}

void test_network_preps(io_uring_sqe& raw_sqe) {
  std::array<char, 16> buffer{};
  sockaddr_storage storage{};
  auto* addr = reinterpret_cast<sockaddr*>(&storage);
  const auto* const_addr = reinterpret_cast<const sockaddr*>(&storage);
  socklen_t addrlen = static_cast<socklen_t>(sizeof(storage));
  msghdr message{};

  bupp::base::submission_queue_entry sqe = reset_sqe(raw_sqe);
  sqe.prep_accept(-1, addr, &addrlen, 0);
  assert(raw_sqe.opcode == IORING_OP_ACCEPT);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_connect(-1, const_addr, addrlen);
  assert(raw_sqe.opcode == IORING_OP_CONNECT);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_send(-1, buffer.data(), buffer.size(), 0);
  assert(raw_sqe.opcode == IORING_OP_SEND);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_sendmsg(-1, &message, 0);
  assert(raw_sqe.opcode == IORING_OP_SENDMSG);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_recv(-1, buffer.data(), buffer.size(), 0);
  assert(raw_sqe.opcode == IORING_OP_RECV);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_recvmsg(-1, &message, 0);
  assert(raw_sqe.opcode == IORING_OP_RECVMSG);
}

void test_poll_and_timeout_preps(io_uring_sqe& raw_sqe) {
  __kernel_timespec timeout{.tv_sec = 0, .tv_nsec = 1000};

  bupp::base::submission_queue_entry sqe = reset_sqe(raw_sqe);
  sqe.prep_poll_add(-1, static_cast<unsigned>(POLLIN));
  assert(raw_sqe.opcode == IORING_OP_POLL_ADD);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_poll_multishot(-1, static_cast<unsigned>(POLLIN));
  assert(raw_sqe.opcode == IORING_OP_POLL_ADD);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_poll_remove(k_user_data);
  assert(raw_sqe.opcode == IORING_OP_POLL_REMOVE);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_poll_update(k_user_data, k_user_data + 1,
                       static_cast<unsigned>(POLLIN), 0);
  assert(raw_sqe.opcode == IORING_OP_POLL_REMOVE);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_timeout(&timeout, 1, 0);
  assert(raw_sqe.opcode == IORING_OP_TIMEOUT);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_timeout_remove(k_user_data, 0);
  assert(raw_sqe.opcode == IORING_OP_TIMEOUT_REMOVE);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_timeout_update(&timeout, k_user_data, 0);
  assert(raw_sqe.opcode == IORING_OP_TIMEOUT_REMOVE);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_cancel(&timeout, 0);
  assert(raw_sqe.opcode == IORING_OP_ASYNC_CANCEL);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_cancel64(k_user_data, 0);
  assert(raw_sqe.opcode == IORING_OP_ASYNC_CANCEL);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_cancel_fd(-1, 0);
  assert(raw_sqe.opcode == IORING_OP_ASYNC_CANCEL);
}

void test_filesystem_preps(io_uring_sqe& raw_sqe) {
  std::array<char, 16> buffer{};
  const unsigned buffer_size = static_cast<unsigned>(buffer.size());
  const int buffer_size_int = static_cast<int>(buffer.size());
  iovec iov{.iov_base = buffer.data(), .iov_len = buffer.size()};
  struct statx statx_buffer{};

  bupp::base::submission_queue_entry sqe = reset_sqe(raw_sqe);
  sqe.prep_read(-1, buffer.data(), buffer_size, 0);
  assert(raw_sqe.opcode == IORING_OP_READ);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_write(-1, buffer.data(), buffer_size, 0);
  assert(raw_sqe.opcode == IORING_OP_WRITE);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_readv(-1, &iov, 1, 0);
  assert(raw_sqe.opcode == IORING_OP_READV);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_writev(-1, &iov, 1, 0);
  assert(raw_sqe.opcode == IORING_OP_WRITEV);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_openat(AT_FDCWD, "/dev/null", O_RDONLY, 0);
  assert(raw_sqe.opcode == IORING_OP_OPENAT);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_open("/dev/null", O_RDONLY, 0);
  assert(raw_sqe.opcode == IORING_OP_OPENAT);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_close(-1);
  assert(raw_sqe.opcode == IORING_OP_CLOSE);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_fsync(-1, 0);
  assert(raw_sqe.opcode == IORING_OP_FSYNC);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_statx(AT_FDCWD, "/dev/null", AT_SYMLINK_NOFOLLOW,
                 static_cast<unsigned>(STATX_BASIC_STATS), &statx_buffer);
  assert(raw_sqe.opcode == IORING_OP_STATX);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_fallocate(-1, 0, 0, 4096);
  assert(raw_sqe.opcode == IORING_OP_FALLOCATE);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_provide_buffers(buffer.data(), buffer_size_int, 1, 7, 0);
  assert(raw_sqe.opcode == IORING_OP_PROVIDE_BUFFERS);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_remove_buffers(1, 7);
  assert(raw_sqe.opcode == IORING_OP_REMOVE_BUFFERS);
}

}  // namespace

int main() {
  io_uring_sqe raw_sqe{};

  bupp::base::submission_queue_entry sqe = reset_sqe(raw_sqe);
  sqe.prep_nop();
  assert(raw_sqe.opcode == IORING_OP_NOP);

  test_metadata_helpers(raw_sqe);
  test_network_preps(raw_sqe);
  test_poll_and_timeout_preps(raw_sqe);
  test_filesystem_preps(raw_sqe);

  return 0;
}
