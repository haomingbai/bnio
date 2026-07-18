#include <bnio/buffer.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

TEST(BufferTest, behavior) {
  std::array<char, 4> array{'a', 'b', 'c', 'd'};
  bnio::mutable_buffer array_buffer = bnio::buffer(array);
  EXPECT_EQ(array_buffer.data(), array.data());
  EXPECT_EQ(array_buffer.size(), array.size());

  const std::string_view text = "hello";
  bnio::const_buffer text_buffer = bnio::buffer(text);
  EXPECT_EQ(text_buffer.data(), text.data());
  EXPECT_EQ(text_buffer.size(), text.size());

  std::string dynamic_storage = "abc";
  auto dynamic = bnio::dynamic_buffer(dynamic_storage);
  bnio::mutable_buffer prepared = dynamic.prepare(4);
  EXPECT_EQ(prepared.size(), 4);
  static_cast<char*>(prepared.data())[0] = 'd';
  static_cast<char*>(prepared.data())[1] = 'e';
  dynamic.commit(2);
  EXPECT_EQ(dynamic_storage, "abcde");
  dynamic.consume(3);
  EXPECT_EQ(dynamic_storage, "de");
  dynamic.consume(dynamic.size());
  EXPECT_TRUE(dynamic_storage.empty());

  std::vector<std::byte> bytes;
  auto byte_buffer = bnio::dynamic_buffer(bytes);
  bnio::mutable_buffer byte_prepared = byte_buffer.prepare(2);
  static_cast<std::byte*>(byte_prepared.data())[0] = std::byte{1};
  byte_buffer.commit(1);
  EXPECT_EQ(bytes.size(), 1);
}
