#include <bupp/base/linux/submission_queue_entry.h>
#include <gtest/gtest.h>
#include <liburing.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <array>
#include <cstdint>

namespace {

constexpr std::uint64_t k_user_data = 0x62757070ULL;

bupp::base::submission_queue_entry reset_sqe(io_uring_sqe& raw_sqe) {
  raw_sqe = {};
  return bupp::base::submission_queue_entry(&raw_sqe);
}

void test_metadata_helpers(io_uring_sqe& raw_sqe) {
  int marker = 0;
  bupp::base::submission_queue_entry sqe = reset_sqe(raw_sqe);

  sqe.set_data(&marker);
  EXPECT_TRUE(raw_sqe.user_data == reinterpret_cast<std::uintptr_t>(&marker));

  sqe.set_data64(k_user_data);
  EXPECT_TRUE(raw_sqe.user_data == k_user_data);
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
  EXPECT_TRUE(raw_sqe.opcode == IORING_OP_ACCEPT);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_connect(-1, const_addr, addrlen);
  EXPECT_TRUE(raw_sqe.opcode == IORING_OP_CONNECT);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_send(-1, buffer.data(), buffer.size(), 0);
  EXPECT_TRUE(raw_sqe.opcode == IORING_OP_SEND);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_sendmsg(-1, &message, 0);
  EXPECT_TRUE(raw_sqe.opcode == IORING_OP_SENDMSG);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_recv(-1, buffer.data(), buffer.size(), 0);
  EXPECT_TRUE(raw_sqe.opcode == IORING_OP_RECV);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_recvmsg(-1, &message, 0);
  EXPECT_TRUE(raw_sqe.opcode == IORING_OP_RECVMSG);
}

void test_poll_and_timeout_preps(io_uring_sqe& raw_sqe) {
  __kernel_timespec timeout{.tv_sec = 0, .tv_nsec = 1000};

  bupp::base::submission_queue_entry sqe = reset_sqe(raw_sqe);
  sqe.prep_poll_add(-1, static_cast<unsigned>(POLLIN));
  EXPECT_TRUE(raw_sqe.opcode == IORING_OP_POLL_ADD);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_timeout(&timeout, 1, 0);
  EXPECT_TRUE(raw_sqe.opcode == IORING_OP_TIMEOUT);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_timeout_update(&timeout, k_user_data, 0);
  EXPECT_TRUE(raw_sqe.opcode == IORING_OP_TIMEOUT_REMOVE);
}

void test_filesystem_preps(io_uring_sqe& raw_sqe) {
  std::array<char, 16> buffer{};
  const unsigned buffer_size = static_cast<unsigned>(buffer.size());
  iovec iov{.iov_base = buffer.data(), .iov_len = buffer.size()};

  bupp::base::submission_queue_entry sqe = reset_sqe(raw_sqe);
  sqe.prep_read(-1, buffer.data(), buffer_size, 0);
  EXPECT_TRUE(raw_sqe.opcode == IORING_OP_READ);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_write(-1, buffer.data(), buffer_size, 0);
  EXPECT_TRUE(raw_sqe.opcode == IORING_OP_WRITE);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_readv(-1, &iov, 1, 0);
  EXPECT_TRUE(raw_sqe.opcode == IORING_OP_READV);

  sqe = reset_sqe(raw_sqe);
  sqe.prep_writev(-1, &iov, 1, 0);
  EXPECT_TRUE(raw_sqe.opcode == IORING_OP_WRITEV);
}

}  // namespace

TEST(SubmissionQueueEntryTest, behavior) {
  io_uring_sqe raw_sqe{};

  bupp::base::submission_queue_entry sqe = reset_sqe(raw_sqe);
  sqe.prep_nop();
  EXPECT_TRUE(raw_sqe.opcode == IORING_OP_NOP);

  test_metadata_helpers(raw_sqe);
  test_network_preps(raw_sqe);
  test_poll_and_timeout_preps(raw_sqe);
  test_filesystem_preps(raw_sqe);
}
