#include <bnio/base/linux/probe.h>
#include <bnio/base/linux/ring.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <utility>

namespace {

bool is_unsupported_ring_error(int result) {
  return result == -ENOSYS || result == -EPERM || result == -EACCES;
}

}  // namespace

TEST(ProbeTest, behavior) {
  bnio::base::probe empty_probe;
  EXPECT_EQ(empty_probe.raw(), nullptr);
  EXPECT_FALSE(empty_probe.is_open());
  EXPECT_EQ(empty_probe.opcode_supported(IORING_OP_NOP), 0);

  bnio::base::probe global_probe;
  static_cast<void>(global_probe.get_probe());
  if (global_probe.is_open()) {
    EXPECT_NE(global_probe.raw(), nullptr);
    static_cast<void>(global_probe.opcode_supported(IORING_OP_NOP));
  }
  global_probe.free_probe();
  EXPECT_FALSE(global_probe.is_open());

  bnio::base::ring ring;
  const int init_result = ring.queue_init(8);
  if (init_result < 0) {
    ASSERT_TRUE(is_unsupported_ring_error(init_result));
    GTEST_SKIP() << "io_uring is unavailable";
  }

  bnio::base::probe ring_probe;
  static_cast<void>(ring_probe.get_probe_ring(ring));
  if (ring_probe.is_open()) {
    EXPECT_NE(ring_probe.raw(), nullptr);
    static_cast<void>(ring_probe.opcode_supported(IORING_OP_NOP));
  }

  bnio::base::probe moved_probe(std::move(ring_probe));
  EXPECT_FALSE(ring_probe.is_open());
  moved_probe.free_probe();
}
