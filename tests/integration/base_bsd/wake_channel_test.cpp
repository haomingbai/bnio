#include <bnio/base/bsd/wake_channel.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <system_error>
#include <thread>
#include <vector>

namespace {

TEST(WakeChannelTest, open_close_lifecycle) {
  bnio::base::wake_channel channel;
  EXPECT_FALSE(channel.is_open());
  EXPECT_EQ(channel.open(), 0);
  EXPECT_TRUE(channel.is_open());
  channel.close();
  EXPECT_FALSE(channel.is_open());
}

TEST(WakeChannelTest, wake_drain_roundtrip) {
  bnio::base::wake_channel channel;
  ASSERT_EQ(channel.open(), 0);
  EXPECT_EQ(channel.wake(), 0);
  EXPECT_EQ(channel.drain(), 0);
}

TEST(WakeChannelTest, wake_before_drain_consumes_all) {
  bnio::base::wake_channel channel;
  ASSERT_EQ(channel.open(), 0);

  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(channel.wake(), 0);
  }

  EXPECT_EQ(channel.drain(), 0);
}

TEST(WakeChannelTest, wake_on_closed_returns_ebadf) {
  bnio::base::wake_channel channel;
  EXPECT_EQ(channel.wake(), -EBADF);
}

TEST(WakeChannelTest, drain_on_closed_returns_ebadf) {
  bnio::base::wake_channel channel;
  EXPECT_EQ(channel.drain(), -EBADF);
}

TEST(WakeChannelTest, move_ownership) {
  bnio::base::wake_channel ch1;
  ASSERT_EQ(ch1.open(), 0);
  EXPECT_TRUE(ch1.is_open());

  bnio::base::wake_channel ch2(std::move(ch1));
  EXPECT_FALSE(ch1.is_open());
  EXPECT_TRUE(ch2.is_open());

  bnio::base::wake_channel ch3;
  ch3 = std::move(ch2);
  EXPECT_FALSE(ch2.is_open());
  EXPECT_TRUE(ch3.is_open());
}

namespace {
void self_move_assign(bnio::base::wake_channel& channel) {
  channel = std::move(channel);
}
}  // namespace

TEST(WakeChannelTest, self_move_assign) {
  bnio::base::wake_channel channel;
  ASSERT_EQ(channel.open(), 0);
  EXPECT_TRUE(channel.is_open());

  self_move_assign(channel);
  EXPECT_TRUE(channel.is_open());

  EXPECT_EQ(channel.wake(), 0);
  EXPECT_EQ(channel.drain(), 0);
}

TEST(WakeChannelTest, read_fd_and_write_fd_different) {
  bnio::base::wake_channel channel;
  ASSERT_EQ(channel.open(), 0);

  EXPECT_NE(channel.read_fd(), channel.write_fd());
}

TEST(WakeChannelTest, close_then_reopen) {
  bnio::base::wake_channel channel;
  ASSERT_EQ(channel.open(), 0);
  EXPECT_TRUE(channel.is_open());

  channel.close();
  EXPECT_FALSE(channel.is_open());

  ASSERT_EQ(channel.open(), 0);
  EXPECT_TRUE(channel.is_open());

  EXPECT_EQ(channel.wake(), 0);
  EXPECT_EQ(channel.drain(), 0);
}

TEST(WakeChannelTest, multi_thread_wake_single_drain) {
  bnio::base::wake_channel channel;
  ASSERT_EQ(channel.open(), 0);

  std::atomic<int> wake_count{0};

  std::thread wake_thread([&channel, &wake_count]() {
    for (int i = 0; i < 100; ++i) {
      EXPECT_EQ(channel.wake(), 0);
      wake_count.fetch_add(1, std::memory_order_relaxed);
    }
  });

  wake_thread.join();
  EXPECT_EQ(wake_count.load(), 100);

  EXPECT_EQ(channel.drain(), 0);
}

TEST(WakeChannelTest, multi_thread_concurrent_wake) {
  bnio::base::wake_channel channel;
  ASSERT_EQ(channel.open(), 0);

  std::atomic<int> wake_count{0};
  std::vector<std::thread> threads;

  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&channel, &wake_count]() {
      for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(channel.wake(), 0);
        wake_count.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(wake_count.load(), 200);
  EXPECT_EQ(channel.drain(), 0);
}

TEST(WakeChannelTest, high_frequency_wake_drain) {
  bnio::base::wake_channel channel;
  ASSERT_EQ(channel.open(), 0);

  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(channel.wake(), 0);
    EXPECT_EQ(channel.drain(), 0);
  }
}

}  // namespace
