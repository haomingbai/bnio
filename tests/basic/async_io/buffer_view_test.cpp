#include <bnio/async_io/buffer_view.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <type_traits>

TEST(BufferViewTest, behavior) {
  static_assert(std::is_trivially_copyable_v<bnio::async_io::buffer_view>);

  char data[8]{};
  bnio::async_io::buffer_view buffer{data, sizeof(data)};

  EXPECT_EQ(buffer.data, data);
  EXPECT_EQ(buffer.size, sizeof(data));
}
