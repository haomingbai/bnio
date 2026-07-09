#include <bupp/base/linux/ring.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace bupp::base {

ring::ring() noexcept = default;

ring::~ring() noexcept { queue_exit(); }

ring::ring(ring&& other) noexcept
    : ring_(std::exchange(other.ring_, {})),
      open_(std::exchange(other.open_, false)) {}

ring& ring::operator=(ring&& other) noexcept {
  if (this != &other) {
    queue_exit();
    ring_ = std::exchange(other.ring_, {});
    open_ = std::exchange(other.open_, false);
  }
  return *this;
}

int ring::queue_init(unsigned entries, unsigned flags) noexcept {
  queue_exit();
  ring_ = {};
  const int result = io_uring_queue_init(entries, &ring_, flags);
  open_ = result >= 0;
  return result;
}

int ring::queue_init_params(unsigned entries, params& queue_params) noexcept {
  queue_exit();
  ring_ = {};
  const int result =
      io_uring_queue_init_params(entries, &ring_, queue_params.raw());
  open_ = result >= 0;
  return result;
}

void ring::queue_exit() noexcept {
  if (open_) {
    io_uring_queue_exit(&ring_);
    ring_ = {};
    open_ = false;
  }
}

int ring::submit() noexcept { return io_uring_submit(&ring_); }

int ring::submit_and_wait(unsigned wait_nr) noexcept {
  return io_uring_submit_and_wait(&ring_, wait_nr);
}

submission_queue_entry ring::get_sqe() noexcept {
  return submission_queue_entry(io_uring_get_sqe(&ring_));
}

int ring::peek_cqe(completion_queue_entry& cqe) noexcept {
  io_uring_cqe* raw_cqe = nullptr;
  const int result = io_uring_peek_cqe(&ring_, &raw_cqe);
  cqe = completion_queue_entry(raw_cqe);
  return result;
}

int ring::wait_cqe(completion_queue_entry& cqe) noexcept {
  io_uring_cqe* raw_cqe = nullptr;
  const int result = io_uring_wait_cqe(&ring_, &raw_cqe);
  cqe = completion_queue_entry(raw_cqe);
  return result;
}

int ring::wait_cqe_timeout(completion_queue_entry& cqe,
                           __kernel_timespec* timeout) noexcept {
  io_uring_cqe* raw_cqe = nullptr;
  const int result = io_uring_wait_cqe_timeout(&ring_, &raw_cqe, timeout);
  cqe = completion_queue_entry(raw_cqe);
  return result;
}

void ring::cqe_seen(completion_queue_entry cqe) noexcept {
  io_uring_cqe_seen(&ring_, cqe.raw());
}

io_uring* ring::raw() noexcept { return &ring_; }

const io_uring* ring::raw() const noexcept { return &ring_; }

int ring::native_fd() const noexcept { return ring_.ring_fd; }

int ring::wait_cqe_event(int ring_fd, unsigned wait_nr) noexcept {
  if (ring_fd < 0) {
    return -EINVAL;
  }
  const int result =
      static_cast<int>(syscall(SYS_io_uring_enter, ring_fd, 0U, wait_nr,
                               IORING_ENTER_GETEVENTS, nullptr, 0U));
  return result < 0 ? -errno : result;
}

bool ring::is_open() const noexcept { return open_; }

}  // namespace bupp::base
