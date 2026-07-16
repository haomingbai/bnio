#include <bupp/async_io/descriptor_view.h>
#include <gtest/gtest.h>

#include <type_traits>

TEST(DescriptorViewTest, behavior) {
  static_assert(sizeof(bupp::async_io::descriptor_view) == sizeof(int));
  static_assert(std::is_trivially_copyable_v<bupp::async_io::descriptor_view>);
  static_assert(std::is_standard_layout_v<bupp::async_io::descriptor_view>);

  bupp::async_io::descriptor_view invalid;
  EXPECT_TRUE(!invalid.valid());
  EXPECT_TRUE(invalid.native_handle() == -1);

  bupp::async_io::descriptor_view descriptor(42);
  EXPECT_TRUE(descriptor.valid());
  EXPECT_TRUE(descriptor.native_handle() == 42);
}
