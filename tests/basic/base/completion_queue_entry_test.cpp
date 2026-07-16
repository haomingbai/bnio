#include <bupp/base/linux/completion_queue_entry.h>
#include <gtest/gtest.h>
#include <liburing.h>

#include <cstdint>

#include "bupp/base/linux/liburing.h"

TEST(CompletionQueueEntryTest, behavior) {
  int marker = 0;
  io_uring_cqe raw_cqe{};
  raw_cqe.user_data = reinterpret_cast<std::uintptr_t>(&marker);
  raw_cqe.res = 7;
  raw_cqe.flags = 3;

  bupp::base::completion_queue_entry cqe(&raw_cqe);

  EXPECT_EQ(cqe.raw(), &raw_cqe);
  EXPECT_EQ(cqe.res(), 7);
  EXPECT_EQ(cqe.flags(), raw_cqe.flags);
  EXPECT_EQ(cqe.get_data(), &marker);
  EXPECT_EQ(cqe.get_data64(), raw_cqe.user_data);
}
