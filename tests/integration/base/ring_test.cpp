#include <bnio/base/linux/completion_queue_entry.h>
#include <bnio/base/linux/liburing.h>
#include <bnio/base/linux/params.h>
#include <bnio/base/linux/ring.h>
#include <bnio/base/linux/submission_queue_entry.h>
#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <thread>
#include <utility>

namespace {

constexpr std::uint64_t k_user_data = 0x62757070ULL;

bool is_unsupported_ring_error(int result) {
  return result == -ENOSYS || result == -EPERM || result == -EACCES;
}

TEST(RingTest, move_closed_ring) {
  bnio::base::ring first;
  bnio::base::ring second(std::move(first));
  EXPECT_FALSE(first.is_open());
  EXPECT_FALSE(second.is_open());

  bnio::base::ring third;
  third = std::move(second);
  EXPECT_FALSE(second.is_open());
  EXPECT_FALSE(third.is_open());
}

TEST(RingTest, nop_round_trip) {
  bnio::base::ring ring;
  bnio::base::params params;

  const int init_result = ring.queue_init_params(8, params);
  if (init_result < 0) {
    ASSERT_TRUE(is_unsupported_ring_error(init_result));
    EXPECT_FALSE(ring.is_open());
    GTEST_SKIP() << "io_uring is unavailable";
  }

  EXPECT_TRUE(ring.is_open());
  EXPECT_NE(ring.raw(), nullptr);

  bnio::base::submission_queue_entry sqe = ring.get_sqe();
  EXPECT_NE(sqe.raw(), nullptr);

  sqe.prep_nop();
  sqe.set_data64(k_user_data);

  const int submit_result = ring.submit();
  EXPECT_TRUE(submit_result >= 0);

  bnio::base::completion_queue_entry cqe;
  const int wait_result = ring.wait_cqe(cqe);
  EXPECT_EQ(wait_result, 0);
  EXPECT_NE(cqe.raw(), nullptr);
  EXPECT_EQ(cqe.res(), 0);
  EXPECT_EQ(cqe.get_data64(), k_user_data);

  ring.cqe_seen(cqe);

  bnio::base::ring moved(std::move(ring));
  EXPECT_FALSE(ring.is_open());
  EXPECT_TRUE(moved.is_open());
}

TEST(RingTest, consume_ready_cqes_respects_window) {
  bnio::base::ring ring;

  const int init_result = ring.queue_init(8);
  if (init_result < 0) {
    ASSERT_TRUE(is_unsupported_ring_error(init_result));
    EXPECT_FALSE(ring.is_open());
    GTEST_SKIP() << "io_uring is unavailable";
  }

  constexpr unsigned k_count = 3;
  for (unsigned index = 0; index < k_count; ++index) {
    bnio::base::submission_queue_entry sqe = ring.get_sqe();
    EXPECT_NE(sqe.raw(), nullptr);
    sqe.prep_nop();
    sqe.set_data64(k_user_data + index);
  }

  const int submit_result = ring.submit_and_wait(k_count);
  EXPECT_TRUE(submit_result >= 0);

  unsigned seen_count = 0;
  std::uint64_t seen_sum = 0;
  const auto record_cqe = [&seen_count, &seen_sum](
                              bnio::base::completion_queue_entry cqe) noexcept {
    EXPECT_NE(cqe.raw(), nullptr);
    EXPECT_EQ(cqe.res(), 0);
    seen_sum += cqe.get_data64();
    ++seen_count;
  };

  const unsigned first_batch = ring.consume_ready_cqes(2, record_cqe);
  EXPECT_EQ(first_batch, 2);
  EXPECT_EQ(seen_count, 2);

  const unsigned second_batch = ring.consume_ready_cqes(2, record_cqe);
  EXPECT_EQ(second_batch, 1);
  EXPECT_EQ(seen_count, k_count);
  EXPECT_EQ(seen_sum, (k_user_data * k_count) + 0 + 1 + 2);
}

TEST(RingTest, disabled_single_issuer_is_claimed_by_enabling_thread) {
  if (bnio::base::detail::io_uring_setup_r_disabled == 0 ||
      bnio::base::detail::io_uring_setup_single_issuer == 0) {
    GTEST_SKIP() << "liburing headers do not expose issuer handoff flags";
  }

  bnio::base::ring ring;
  bnio::base::params params;
  params.set_flags(bnio::base::detail::io_uring_setup_r_disabled |
                   bnio::base::detail::io_uring_setup_single_issuer);
  const int init_result = ring.queue_init_params(8, params);
  if (init_result == -EINVAL) {
    GTEST_SKIP() << "kernel does not support issuer handoff flags";
  }
  if (init_result < 0) {
    ASSERT_TRUE(is_unsupported_ring_error(init_result));
    GTEST_SKIP() << "io_uring is unavailable";
  }

  int enable_result = -1;
  int submit_result = -1;
  int wait_result = -1;
  int cqe_result = -1;
  std::uint64_t cqe_data = 0;
  std::thread issuer([&] {
    enable_result = ring.enable();
    if (enable_result < 0) {
      return;
    }

    bnio::base::submission_queue_entry sqe = ring.get_sqe();
    if (sqe.raw() == nullptr) {
      return;
    }
    sqe.prep_nop();
    sqe.set_data64(k_user_data);
    submit_result = ring.submit();
    if (submit_result <= 0) {
      return;
    }

    bnio::base::completion_queue_entry cqe;
    wait_result = ring.wait_cqe(cqe);
    if (wait_result < 0 || cqe.raw() == nullptr) {
      return;
    }
    cqe_result = cqe.res();
    cqe_data = cqe.get_data64();
    ring.cqe_seen(cqe);
  });
  issuer.join();

  EXPECT_EQ(enable_result, 0);
  EXPECT_GT(submit_result, 0);
  EXPECT_EQ(wait_result, 0);
  EXPECT_EQ(cqe_result, 0);
  EXPECT_EQ(cqe_data, k_user_data);
}

}  // namespace
