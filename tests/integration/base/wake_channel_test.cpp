#include <bnio/base/linux/wake_channel.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>

namespace {

TEST(WakeChannelTest, wake_on_closed_returns_ebadf) {
  bnio::base::wake_channel channel;
  EXPECT_EQ(channel.wake(), -EBADF);
}

TEST(WakeChannelTest, drain_on_closed_returns_ebadf) {
  bnio::base::wake_channel channel;
  EXPECT_EQ(channel.drain(), -EBADF);
}

TEST(WakeChannelTest, wake_eagain_treated_as_success) {
  bnio::base::wake_channel channel;
  ASSERT_EQ(channel.open(), 0);

  // Fill the eventfd counter to its maximum so the next write returns
  // EAGAIN.  The counter max is UINT64_MAX - 1.
  const std::uint64_t max_val = UINT64_MAX - 1;
  const ssize_t written = ::write(channel.read_fd(), &max_val, sizeof(max_val));
  ASSERT_GT(written, 0);

  // wake() tries to add 1, which would exceed UINT64_MAX - 1, so the
  // non-blocking eventfd returns EAGAIN.  The implementation treats
  // EAGAIN as success (workers are already awake).
  EXPECT_EQ(channel.wake(), 0);

  // Drain to reset the counter.
  EXPECT_EQ(channel.drain(), 0);
}

}  // namespace
