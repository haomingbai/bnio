#include <bupp/buffer.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

TEST(BufferTest, behavior) {
  std::array<char, 4> array{'a', 'b', 'c', 'd'};
  bupp::mutable_buffer array_buffer = bupp::buffer(array);
  EXPECT_TRUE(array_buffer.data() == array.data());
  EXPECT_TRUE(array_buffer.size() == array.size());

  const std::string_view text = "hello";
  bupp::const_buffer text_buffer = bupp::buffer(text);
  EXPECT_TRUE(text_buffer.data() == text.data());
  EXPECT_TRUE(text_buffer.size() == text.size());

  std::string dynamic_storage = "abc";
  auto dynamic = bupp::dynamic_buffer(dynamic_storage);
  bupp::mutable_buffer prepared = dynamic.prepare(4);
  EXPECT_TRUE(prepared.size() == 4);
  static_cast<char*>(prepared.data())[0] = 'd';
  static_cast<char*>(prepared.data())[1] = 'e';
  dynamic.commit(2);
  EXPECT_TRUE(dynamic_storage == "abcde");
  dynamic.consume(3);
  EXPECT_TRUE(dynamic_storage == "de");
  dynamic.consume(dynamic.size());
  EXPECT_TRUE(dynamic_storage.empty());

  std::vector<std::byte> bytes;
  auto byte_buffer = bupp::dynamic_buffer(bytes);
  bupp::mutable_buffer byte_prepared = byte_buffer.prepare(2);
  static_cast<std::byte*>(byte_prepared.data())[0] = std::byte{1};
  byte_buffer.commit(1);
  EXPECT_TRUE(bytes.size() == 1);
}
