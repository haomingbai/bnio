#pragma once
#ifndef BNIO_EXAMPLES_BASE_EXAMPLE_SUPPORT_H_
#define BNIO_EXAMPLES_BASE_EXAMPLE_SUPPORT_H_

#include <bnio/base/linux/completion_queue_entry.h>
#include <bnio/base/linux/ring.h>
#include <bnio/base/linux/submission_queue_entry.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>

namespace bnio::examples::base {

/**
 * RAII owner for a POSIX file descriptor used by examples.
 *
 * @see close
 */
class unique_fd {
 public:
  /**
   * Creates an empty file descriptor owner.
   */
  unique_fd() noexcept = default;

  /**
   * Takes ownership of a file descriptor.
   */
  explicit unique_fd(int fd) noexcept : fd_(fd) {}

  /**
   * Closes the owned file descriptor, if any.
   *
   * @see close
   */
  ~unique_fd() noexcept { reset(); }

  /**
   * Copy construction is disabled because unique_fd owns a descriptor.
   */
  unique_fd(const unique_fd&) = delete;

  /**
   * Copy assignment is disabled because unique_fd owns a descriptor.
   */
  unique_fd& operator=(const unique_fd&) = delete;

  /**
   * Moves ownership of a file descriptor.
   */
  unique_fd(unique_fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

  /**
   * Moves ownership of a file descriptor.
   */
  unique_fd& operator=(unique_fd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }

  /**
   * Returns the owned file descriptor, or -1 when empty.
   */
  [[nodiscard]] int get() const noexcept { return fd_; }

  /**
   * Returns whether this object owns a descriptor.
   */
  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

  /**
   * Releases ownership and returns the previous descriptor.
   */
  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

  /**
   * Closes the current descriptor and optionally takes ownership of another.
   *
   * @see close
   */
  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      static_cast<void>(::close(fd_));
    }
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

/**
 * Result of trying to initialize an example io_uring instance.
 */
enum class ring_init_result {
  /**
   * The ring was initialized successfully.
   */
  ready,

  /**
   * io_uring is not available on the current host or permissions.
   */
  unavailable,

  /**
   * Ring initialization failed for another reason.
   */
  failed,
};

/**
 * Returns whether a ring initialization result indicates unavailable io_uring.
 */
[[nodiscard]] inline bool is_ring_unavailable_result(int result) noexcept {
  return result == -ENOSYS || result == -EPERM || result == -EACCES;
}

/**
 * Returns whether a feature result indicates an unavailable host capability.
 */
[[nodiscard]] inline bool is_feature_unavailable_result(int result) noexcept {
  return is_ring_unavailable_result(result) || result == -EINVAL ||
         result == -EOPNOTSUPP;
}

/**
 * Initializes a ring and logs example-friendly diagnostics on failure.
 */
[[nodiscard]] inline ring_init_result init_ring(bnio::base::ring& ring,
                                                unsigned entries,
                                                std::string_view name) {
  const int result = ring.queue_init(entries);
  if (result >= 0) {
    return ring_init_result::ready;
  }

  if (is_ring_unavailable_result(result)) {
    std::cerr << name << ": io_uring is not available: " << result << '\n';
    return ring_init_result::unavailable;
  }

  std::cerr << name << ": io_uring_queue_init failed: " << result << '\n';
  return ring_init_result::failed;
}

/**
 * Returns an SQE or logs that the submission queue is full.
 */
[[nodiscard]] inline bnio::base::submission_queue_entry get_sqe_or_log(
    bnio::base::ring& ring, std::string_view label) {
  bnio::base::submission_queue_entry sqe = ring.get_sqe();
  if (sqe.raw() == nullptr) {
    std::cerr << label << ": submission queue is full\n";
  }
  return sqe;
}

/**
 * Submits pending SQEs and waits for one expected completion.
 */
[[nodiscard]] inline int submit_and_wait_one(bnio::base::ring& ring,
                                             std::uint64_t expected_user_data,
                                             std::string_view label) {
  const int submit_result = ring.submit();
  if (submit_result < 0) {
    std::cerr << label << ": io_uring_submit failed: " << submit_result << '\n';
    return submit_result;
  }

  bnio::base::completion_queue_entry cqe;
  const int wait_result = ring.wait_cqe(cqe);
  if (wait_result < 0) {
    std::cerr << label << ": io_uring_wait_cqe failed: " << wait_result << '\n';
    return wait_result;
  }

  const int result = cqe.res();
  const std::uint64_t user_data = cqe.get_data64();
  ring.cqe_seen(cqe);

  if (user_data != expected_user_data) {
    std::cerr << label << ": unexpected user_data=" << user_data << '\n';
    return -EINVAL;
  }

  return result;
}

}  // namespace bnio::examples::base

#endif  // BNIO_EXAMPLES_BASE_EXAMPLE_SUPPORT_H_
