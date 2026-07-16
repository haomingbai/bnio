#pragma once
#ifndef BUPP_BASE_LINUX_LIBURING_H_
#define BUPP_BASE_LINUX_LIBURING_H_

#if defined(__has_include)
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#endif
#endif

#include <liburing.h>

#include <cstdint>

namespace bupp::base::detail {

#ifdef IORING_SETUP_COOP_TASKRUN
inline constexpr unsigned io_uring_setup_coop_taskrun =
    IORING_SETUP_COOP_TASKRUN;
#else
inline constexpr unsigned io_uring_setup_coop_taskrun = 0;
#endif

#ifdef IORING_SETUP_SINGLE_ISSUER
inline constexpr unsigned io_uring_setup_single_issuer =
    IORING_SETUP_SINGLE_ISSUER;
#else
inline constexpr unsigned io_uring_setup_single_issuer = 0;
#endif

inline void io_uring_sqe_set_data64(io_uring_sqe* sqe,
                                    std::uint64_t data) noexcept {
  sqe->user_data = data;
}

[[nodiscard]] inline std::uint64_t io_uring_cqe_get_data64(
    const io_uring_cqe* cqe) noexcept {
  return cqe->user_data;
}

}  // namespace bupp::base::detail

#endif  // BUPP_BASE_LINUX_LIBURING_H_
