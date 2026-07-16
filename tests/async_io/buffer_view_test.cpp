#include <bupp/async_io/buffer_view.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <type_traits>

TEST(BufferViewTest, behavior) {
  static_assert(std::is_trivially_copyable_v<bupp::async_io::buffer_view>);

  char data[8]{};
  bupp::async_io::buffer_view buffer{data, sizeof(data)};

  EXPECT_TRUE(buffer.data == data);
  EXPECT_TRUE(buffer.size == sizeof(data));
}
