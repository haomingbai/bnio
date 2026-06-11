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
  io_uring_sqe_set_data64(sqe_, data);
}

void submission_queue_entry::set_flags(unsigned flags) noexcept {
  io_uring_sqe_set_flags(sqe_, flags);
}

void submission_queue_entry::set_buf_group(int bgid) noexcept {
  io_uring_sqe_set_buf_group(sqe_, bgid);
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

void submission_queue_entry::prep_poll_multishot(int fd,
                                                 unsigned poll_mask) noexcept {
  io_uring_prep_poll_multishot(sqe_, fd, poll_mask);
}

void submission_queue_entry::prep_poll_remove(
    std::uint64_t user_data) noexcept {
  io_uring_prep_poll_remove(sqe_, user_data);
}

void submission_queue_entry::prep_poll_update(std::uint64_t old_user_data,
                                              std::uint64_t new_user_data,
                                              unsigned poll_mask,
                                              unsigned flags) noexcept {
  io_uring_prep_poll_update(sqe_, old_user_data, new_user_data, poll_mask,
                            flags);
}

void submission_queue_entry::prep_timeout(const __kernel_timespec* ts,
                                          unsigned count,
                                          unsigned flags) noexcept {
  io_uring_prep_timeout(sqe_, ts, count, flags);
}

void submission_queue_entry::prep_timeout_remove(std::uint64_t user_data,
                                                 unsigned flags) noexcept {
  io_uring_prep_timeout_remove(sqe_, user_data, flags);
}

void submission_queue_entry::prep_timeout_update(const __kernel_timespec* ts,
                                                 std::uint64_t user_data,
                                                 unsigned flags) noexcept {
  io_uring_prep_timeout_update(sqe_, ts, user_data, flags);
}

void submission_queue_entry::prep_cancel(const void* user_data,
                                         int flags) noexcept {
  io_uring_prep_cancel(sqe_, user_data, flags);
}

void submission_queue_entry::prep_cancel64(std::uint64_t user_data,
                                           int flags) noexcept {
  io_uring_prep_cancel64(sqe_, user_data, flags);
}

void submission_queue_entry::prep_cancel_fd(int fd, unsigned flags) noexcept {
  io_uring_prep_cancel_fd(sqe_, fd, flags);
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

void submission_queue_entry::prep_openat(int dfd, const char* path, int flags,
                                         mode_t mode) noexcept {
  io_uring_prep_openat(sqe_, dfd, path, flags, mode);
}

void submission_queue_entry::prep_open(const char* path, int flags,
                                       mode_t mode) noexcept {
  io_uring_prep_open(sqe_, path, flags, mode);
}

void submission_queue_entry::prep_close(int fd) noexcept {
  io_uring_prep_close(sqe_, fd);
}

void submission_queue_entry::prep_fsync(int fd, unsigned fsync_flags) noexcept {
  io_uring_prep_fsync(sqe_, fd, fsync_flags);
}

void submission_queue_entry::prep_statx(int dfd, const char* path, int flags,
                                        unsigned mask,
                                        struct statx* statxbuf) noexcept {
  io_uring_prep_statx(sqe_, dfd, path, flags, mask, statxbuf);
}

void submission_queue_entry::prep_fallocate(int fd, int mode,
                                            std::uint64_t offset,
                                            std::uint64_t len) noexcept {
  io_uring_prep_fallocate(sqe_, fd, mode, offset, len);
}

void submission_queue_entry::prep_provide_buffers(void* addr, int len, int nr,
                                                  int bgid, int bid) noexcept {
  io_uring_prep_provide_buffers(sqe_, addr, len, nr, bgid, bid);
}

void submission_queue_entry::prep_remove_buffers(int nr, int bgid) noexcept {
  io_uring_prep_remove_buffers(sqe_, nr, bgid);
}

}  // namespace bupp::base
