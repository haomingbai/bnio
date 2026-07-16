#include <bupp/base/linux/liburing.h>
#include <bupp/base/linux/params.h>
#include <gtest/gtest.h>

TEST(ParamsTest, behavior) {
  bupp::base::params params;

  EXPECT_TRUE(params.raw() != nullptr);
  EXPECT_TRUE(params.sq_entries() == 0);
  EXPECT_TRUE(params.cq_entries() == 0);
  EXPECT_TRUE(params.flags() == 0);
  EXPECT_TRUE(params.sq_thread_cpu() == 0);
  EXPECT_TRUE(params.sq_thread_idle() == 0);
  EXPECT_TRUE(params.features() == 0);
  EXPECT_TRUE(params.wq_fd() == 0);

  params.set_sq_entries(8);
  params.set_cq_entries(16);
  params.set_flags(IORING_SETUP_CLAMP);
  params.set_sq_thread_cpu(1);
  params.set_sq_thread_idle(10);
  params.set_features(IORING_FEAT_NODROP);
  params.set_wq_fd(2);

  EXPECT_TRUE(params.sq_entries() == 8);
  EXPECT_TRUE(params.cq_entries() == 16);
  EXPECT_TRUE(params.flags() == IORING_SETUP_CLAMP);
  EXPECT_TRUE(params.sq_thread_cpu() == 1);
  EXPECT_TRUE(params.sq_thread_idle() == 10);
  EXPECT_TRUE(params.features() == IORING_FEAT_NODROP);
  EXPECT_TRUE(params.wq_fd() == 2);

  params.reset();
  EXPECT_TRUE(params.sq_entries() == 0);
  EXPECT_TRUE(params.cq_entries() == 0);
  EXPECT_TRUE(params.flags() == 0);
  EXPECT_TRUE(params.features() == 0);
}
