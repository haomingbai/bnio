#include <bnio/base/bsd/wake_channel.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

TEST(WakeChannelStressTest, concurrent_wake_contention) {
  bnio::base::wake_channel channel;
  ASSERT_EQ(channel.open(), 0);

  std::atomic<int> total_wakes{0};
  std::atomic<bool> stop{false};

  std::vector<std::thread> writers;
  for (int t = 0; t < 8; ++t) {
    writers.emplace_back([&channel, &total_wakes, &stop]() {
      for (int i = 0; i < 10000; ++i) {
        if (stop.load(std::memory_order_relaxed)) break;
        EXPECT_EQ(channel.wake(), 0);
        total_wakes.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::thread drainer([&channel, &stop]() {
    while (!stop.load(std::memory_order_relaxed)) {
      channel.drain();
    }
    channel.drain();
  });

  for (auto& t : writers) {
    t.join();
  }

  stop.store(true, std::memory_order_relaxed);
  drainer.join();

  EXPECT_EQ(total_wakes.load(), 80000);
}

TEST(WakeChannelStressTest, drain_after_wake_race) {
  bnio::base::wake_channel channel;
  ASSERT_EQ(channel.open(), 0);

  std::atomic<int> wake_count{0};
  std::atomic<int> drain_count{0};
  std::atomic<int> failures{0};

  std::thread waker([&channel, &wake_count, &failures]() {
    for (int i = 0; i < 50000; ++i) {
      if (channel.wake() != 0) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
      wake_count.fetch_add(1, std::memory_order_relaxed);
    }
  });

  std::thread drainer_thread([&channel, &drain_count, &failures]() {
    for (int i = 0; i < 50000; ++i) {
      if (channel.drain() != 0) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
      drain_count.fetch_add(1, std::memory_order_relaxed);
    }
  });

  waker.join();
  drainer_thread.join();

  channel.drain();

  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(wake_count.load(), 50000);
}

TEST(WakeChannelStressTest, sustained_cycle) {
  bnio::base::wake_channel channel;
  ASSERT_EQ(channel.open(), 0);

  std::atomic<int> iterations{0};
  std::atomic<int> failures{0};
  std::atomic<bool> stop{false};

  std::thread worker([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      if (channel.wake() != 0) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
      if (channel.drain() != 0) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
      iterations.fetch_add(1, std::memory_order_relaxed);
    }
  });

  std::this_thread::sleep_for(std::chrono::seconds(5));
  stop.store(true, std::memory_order_relaxed);
  worker.join();

  EXPECT_EQ(failures.load(), 0);
  EXPECT_GT(iterations.load(), 0);
}

}  // namespace
