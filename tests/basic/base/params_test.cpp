#include <bnio/base/linux/liburing.h>
#include <bnio/base/linux/params.h>
#include <gtest/gtest.h>

TEST(ParamsTest, behavior) {
  bnio::base::params params;

  EXPECT_NE(params.raw(), nullptr);
  EXPECT_EQ(params.sq_entries(), 0);
  EXPECT_EQ(params.cq_entries(), 0);
  EXPECT_EQ(params.flags(), 0);
  EXPECT_EQ(params.sq_thread_cpu(), 0);
  EXPECT_EQ(params.sq_thread_idle(), 0);
  EXPECT_EQ(params.features(), 0);
  EXPECT_EQ(params.wq_fd(), 0);

  params.set_sq_entries(8);
  params.set_cq_entries(16);
  params.set_flags(IORING_SETUP_CLAMP);
  params.set_sq_thread_cpu(1);
  params.set_sq_thread_idle(10);
  params.set_features(IORING_FEAT_NODROP);
  params.set_wq_fd(2);

  EXPECT_EQ(params.sq_entries(), 8);
  EXPECT_EQ(params.cq_entries(), 16);
  EXPECT_EQ(params.flags(), IORING_SETUP_CLAMP);
  EXPECT_EQ(params.sq_thread_cpu(), 1);
  EXPECT_EQ(params.sq_thread_idle(), 10);
  EXPECT_EQ(params.features(), IORING_FEAT_NODROP);
  EXPECT_EQ(params.wq_fd(), 2);

  params.reset();
  EXPECT_EQ(params.sq_entries(), 0);
  EXPECT_EQ(params.cq_entries(), 0);
  EXPECT_EQ(params.flags(), 0);
  EXPECT_EQ(params.features(), 0);
}
