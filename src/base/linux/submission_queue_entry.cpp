#include <bupp/base/linux/submission_queue_entry.h>

namespace bupp::base {

submission_queue_entry::submission_queue_entry() noexcept = default;

submission_queue_entry::submission_queue_entry(io_uring_sqe* sqe) noexcept
    : sqe_(sqe) {}

io_uring_sqe* submission_queue_entry::raw() noexcept { return sqe_; }

const io_uring_sqe* submission_queue_entry::raw() const noexcept {
  return sqe_;
}

void submission_queue_entry::set_data(void* data) noexcept {
  io_uring_sqe_set_data(sqe_, data);
}

void submission_queue_entry::set_data64(std::uint64_t data) noexcept {
  detail::io_uring_sqe_set_data64(sqe_, data);
}

void submission_queue_entry::prep_nop() noexcept { io_uring_prep_nop(sqe_); }

void submission_queue_entry::prep_accept(int fd, sockaddr* addr,
                                         socklen_t* addrlen,
                                         int flags) noexcept {
  io_uring_prep_accept(sqe_, fd, addr, addrlen, flags);
}

void submission_queue_entry::prep_connect(int fd, const sockaddr* addr,
                                          socklen_t addrlen) noexcept {
  io_uring_prep_connect(sqe_, fd, addr, addrlen);
}

void submission_queue_entry::prep_send(int sockfd, const void* buf,
                                       std::size_t len, int flags) noexcept {
  io_uring_prep_send(sqe_, sockfd, buf, len, flags);
}

void submission_queue_entry::prep_sendmsg(int fd, const msghdr* msg,
                                          unsigned flags) noexcept {
  io_uring_prep_sendmsg(sqe_, fd, msg, flags);
}

void submission_queue_entry::prep_recv(int sockfd, void* buf, std::size_t len,
                                       int flags) noexcept {
  io_uring_prep_recv(sqe_, sockfd, buf, len, flags);
}

void submission_queue_entry::prep_recvmsg(int fd, msghdr* msg,
                                          unsigned flags) noexcept {
  io_uring_prep_recvmsg(sqe_, fd, msg, flags);
}

void submission_queue_entry::prep_poll_add(int fd,
                                           unsigned poll_mask) noexcept {
  io_uring_prep_poll_add(sqe_, fd, poll_mask);
}

void submission_queue_entry::prep_timeout(__kernel_timespec* ts,
                                          unsigned count,
                                          unsigned flags) noexcept {
  io_uring_prep_timeout(sqe_, ts, count, flags);
}

void submission_queue_entry::prep_timeout_update(__kernel_timespec* ts,
                                                 std::uint64_t user_data,
                                                 unsigned flags) noexcept {
  io_uring_prep_timeout_update(sqe_, ts, user_data, flags);
}

void submission_queue_entry::prep_read(int fd, void* buf, unsigned nbytes,
                                       std::uint64_t offset) noexcept {
  io_uring_prep_read(sqe_, fd, buf, nbytes, offset);
}

void submission_queue_entry::prep_write(int fd, const void* buf,
                                        unsigned nbytes,
                                        std::uint64_t offset) noexcept {
  io_uring_prep_write(sqe_, fd, buf, nbytes, offset);
}

void submission_queue_entry::prep_readv(int fd, const iovec* iovecs,
                                        unsigned nr_vecs,
                                        std::uint64_t offset) noexcept {
  io_uring_prep_readv(sqe_, fd, iovecs, nr_vecs, offset);
}

void submission_queue_entry::prep_writev(int fd, const iovec* iovecs,
                                         unsigned nr_vecs,
                                         std::uint64_t offset) noexcept {
  io_uring_prep_writev(sqe_, fd, iovecs, nr_vecs, offset);
}

}  // namespace bupp::base
