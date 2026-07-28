#include <bnio/async_io/buffer_view.h>
#include <bnio/buffer.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
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

// --- mutable_buffer_holder / const_buffer_holder ---

TEST(BufferTest, mutable_buffer_holder_basic) {
  char data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  bnio::mutable_buffer buf(data, sizeof(data));
  bnio::detail::mutable_buffer_holder holder(buf);

  bnio::async_io::buffer_view view = holder.view();
  EXPECT_EQ(view.data, data);
  EXPECT_EQ(view.size, sizeof(data));

  holder.commit(4);

  view = holder.view();
  EXPECT_EQ(view.data, data);
  EXPECT_EQ(view.size, sizeof(data));
}

TEST(BufferTest, const_buffer_holder_basic) {
  const char data[] = "hello";
  bnio::const_buffer buf(data, sizeof(data));
  bnio::detail::const_buffer_holder holder(buf);

  EXPECT_EQ(holder.data(), data);
  EXPECT_EQ(holder.size(), sizeof(data));
}

// --- dynamic_buffer_holder ---

TEST(BufferTest, dynamic_buffer_holder_prepare_commit) {
  std::vector<std::byte> vec;
  bnio::dynamic_byte_vector_buffer dyn_buf(vec);
  bnio::detail::dynamic_buffer_holder<bnio::dynamic_byte_vector_buffer<>>
      holder(dyn_buf, 16);

  bnio::async_io::buffer_view view = holder.view();
  EXPECT_NE(view.data, nullptr);
  EXPECT_EQ(view.size, 16);

  static_cast<std::byte*>(view.data)[0] = std::byte{42};
  holder.commit(4);

  EXPECT_EQ(vec.size(), 4);
  EXPECT_EQ(vec[0], std::byte{42});

  auto data = dyn_buf.data();
  EXPECT_EQ(data.size(), 4);
}

TEST(BufferTest, dynamic_buffer_holder_consume) {
  std::vector<std::byte> vec;
  bnio::dynamic_byte_vector_buffer dyn_buf(vec);
  bnio::detail::dynamic_buffer_holder<bnio::dynamic_byte_vector_buffer<>>
      holder(dyn_buf, 8);

  auto view = holder.view();
  for (std::size_t i = 0; i < 4; ++i) {
    static_cast<std::byte*>(view.data)[i] =
        std::byte{static_cast<unsigned char>(i + 1)};
  }
  holder.commit(4);
  EXPECT_EQ(vec.size(), 4);

  dyn_buf.consume(2);
  EXPECT_EQ(dyn_buf.size(), 2);

  auto data = dyn_buf.data();
  EXPECT_EQ(data.size(), 2);
  const auto* bytes = static_cast<const std::byte*>(data.data());
  EXPECT_EQ(bytes[0], std::byte{3});
  EXPECT_EQ(bytes[1], std::byte{4});
}

// --- make_mutable_buffer_holder ---

TEST(BufferTest, make_mutable_buffer_holder_from_mutable_buffer) {
  char data[4] = {1, 2, 3, 4};
  bnio::mutable_buffer buf(data, sizeof(data));
  auto holder = bnio::detail::make_mutable_buffer_holder(buf);

  auto view = holder.view();
  EXPECT_EQ(view.data, data);
  EXPECT_EQ(view.size, sizeof(data));
}

TEST(BufferTest, make_mutable_buffer_holder_from_buffer_view) {
  char data[4] = {1, 2, 3, 4};
  bnio::async_io::buffer_view buf_view{data, sizeof(data)};
  auto holder = bnio::detail::make_mutable_buffer_holder(buf_view);

  auto view = holder.view();
  EXPECT_EQ(view.data, data);
  EXPECT_EQ(view.size, sizeof(data));
}

TEST(BufferTest, make_mutable_buffer_holder_from_dynamic_vector) {
  std::vector<std::byte> vec;
  bnio::dynamic_byte_vector_buffer dyn_buf(vec);
  auto holder = bnio::detail::make_mutable_buffer_holder(dyn_buf);

  auto view = holder.view();
  EXPECT_NE(view.data, nullptr);
  EXPECT_EQ(view.size, 4096);
}

TEST(BufferTest, make_mutable_buffer_holder_from_dynamic_string) {
  std::string str;
  bnio::dynamic_string_buffer dyn_buf(str);
  auto holder = bnio::detail::make_mutable_buffer_holder(dyn_buf);

  auto view = holder.view();
  EXPECT_NE(view.data, nullptr);
  EXPECT_EQ(view.size, 4096);
}

// --- make_const_buffer_holder ---

TEST(BufferTest, make_const_buffer_holder_from_const_buffer) {
  const char data[] = "test";
  bnio::const_buffer buf(data, sizeof(data));
  auto holder = bnio::detail::make_const_buffer_holder(buf);

  EXPECT_EQ(holder.data(), data);
  EXPECT_EQ(holder.size(), sizeof(data));
}

TEST(BufferTest, make_const_buffer_holder_from_mutable_buffer) {
  char data[4] = {1, 2, 3, 4};
  bnio::mutable_buffer buf(data, sizeof(data));
  auto holder = bnio::detail::make_const_buffer_holder(buf);

  EXPECT_EQ(holder.data(), data);
  EXPECT_EQ(holder.size(), sizeof(data));
}

TEST(BufferTest, make_const_buffer_holder_from_buffer_view) {
  char data[4] = {1, 2, 3, 4};
  bnio::async_io::buffer_view buf_view{data, sizeof(data)};
  auto holder = bnio::detail::make_const_buffer_holder(buf_view);

  EXPECT_EQ(holder.data(), data);
  EXPECT_EQ(holder.size(), sizeof(data));
}

// --- buffer() factory functions ---

TEST(BufferTest, buffer_from_void_ptr) {
  int value = 42;
  bnio::mutable_buffer buf =
      bnio::buffer(static_cast<void*>(&value), sizeof(int));

  EXPECT_EQ(buf.data(), &value);
  EXPECT_EQ(buf.size(), sizeof(int));
}

TEST(BufferTest, buffer_from_const_void_ptr) {
  const int value = 42;
  bnio::const_buffer buf =
      bnio::buffer(static_cast<const void*>(&value), sizeof(int));

  EXPECT_EQ(buf.data(), &value);
  EXPECT_EQ(buf.size(), sizeof(int));
}

TEST(BufferTest, buffer_from_vector) {
  std::vector<char> vec = {'a', 'b', 'c'};
  bnio::mutable_buffer buf = bnio::buffer(vec);

  EXPECT_EQ(buf.data(), vec.data());
  EXPECT_EQ(buf.size(), vec.size() * sizeof(char));
}

TEST(BufferTest, buffer_from_const_vector) {
  const std::vector<char> vec = {'x', 'y', 'z'};
  bnio::const_buffer buf = bnio::buffer(vec);

  EXPECT_EQ(buf.data(), vec.data());
  EXPECT_EQ(buf.size(), vec.size() * sizeof(char));
}

TEST(BufferTest, buffer_from_string) {
  std::string str = "hello world";
  bnio::mutable_buffer buf = bnio::buffer(str);

  EXPECT_EQ(buf.data(), str.data());
  EXPECT_EQ(buf.size(), str.size());
}

TEST(BufferTest, buffer_from_const_string_view) {
  std::string_view sv = "hello";
  bnio::const_buffer buf = bnio::buffer(sv);

  EXPECT_EQ(buf.data(), sv.data());
  EXPECT_EQ(buf.size(), sv.size());
}

TEST(BufferTest, buffer_from_span) {
  char arr[] = {'a', 'b', 'c', 'd'};
  std::span<char> sp(arr);
  bnio::mutable_buffer buf = bnio::buffer(sp);

  EXPECT_EQ(buf.data(), arr);
  EXPECT_EQ(buf.size(), sizeof(arr));
}

TEST(BufferTest, buffer_from_array) {
  std::array<char, 4> arr = {'a', 'b', 'c', 'd'};
  bnio::mutable_buffer buf = bnio::buffer(arr);

  EXPECT_EQ(buf.data(), arr.data());
  EXPECT_EQ(buf.size(), sizeof(arr));
}

TEST(BufferTest, buffer_empty_container) {
  std::vector<char> empty_vec;
  bnio::mutable_buffer vec_buf = bnio::buffer(empty_vec);
  EXPECT_EQ(vec_buf.size(), 0);

  std::string empty_str;
  bnio::mutable_buffer str_buf = bnio::buffer(empty_str);
  EXPECT_EQ(str_buf.size(), 0);
}

// --- dynamic_byte_vector_buffer / dynamic_string_buffer ---

TEST(BufferTest, dynamic_byte_vector_buffer_basic) {
  std::vector<std::byte> vec;
  bnio::dynamic_byte_vector_buffer dyn_buf(vec);

  EXPECT_EQ(dyn_buf.size(), 0);

  auto prepared = dyn_buf.prepare(4);
  EXPECT_EQ(prepared.size(), 4);
  EXPECT_NE(prepared.data(), nullptr);

  static_cast<std::byte*>(prepared.data())[0] = std::byte{10};
  static_cast<std::byte*>(prepared.data())[1] = std::byte{20};
  dyn_buf.commit(2);

  EXPECT_EQ(dyn_buf.size(), 2);
  EXPECT_EQ(vec.size(), 2);

  auto data = dyn_buf.data();
  EXPECT_EQ(data.size(), 2);
  const auto* bytes = static_cast<const std::byte*>(data.data());
  EXPECT_EQ(bytes[0], std::byte{10});
  EXPECT_EQ(bytes[1], std::byte{20});
}

TEST(BufferTest, dynamic_byte_vector_buffer_multiple_prepare_without_commit) {
  std::vector<std::byte> vec;
  bnio::dynamic_byte_vector_buffer dyn_buf(vec);

  auto first = dyn_buf.prepare(8);
  EXPECT_EQ(first.size(), 8);
  EXPECT_EQ(vec.size(), 8);

  auto second = dyn_buf.prepare(16);
  EXPECT_EQ(second.size(), 16);
  EXPECT_EQ(second.data(),
            static_cast<void*>(vec.data() + 8));
  EXPECT_EQ(vec.size(), 24);

  static_cast<std::byte*>(second.data())[0] = std::byte{99};
  dyn_buf.commit(4);

  EXPECT_EQ(dyn_buf.size(), 12);
  EXPECT_EQ(vec.size(), 12);
  EXPECT_EQ(vec[8], std::byte{99});
}

TEST(BufferTest, dynamic_byte_vector_buffer_max_size) {
  std::vector<std::byte> vec;
  EXPECT_GE(vec.max_size(), 1024);

  bnio::dynamic_byte_vector_buffer dyn_buf(vec);
  EXPECT_EQ(dyn_buf.size(), 0);
}

TEST(BufferTest, dynamic_string_buffer_basic) {
  std::string str;
  bnio::dynamic_string_buffer dyn_buf(str);

  EXPECT_EQ(dyn_buf.size(), 0);

  auto prepared = dyn_buf.prepare(5);
  EXPECT_EQ(prepared.size(), 5);
  EXPECT_NE(prepared.data(), nullptr);

  static_cast<char*>(prepared.data())[0] = 'h';
  static_cast<char*>(prepared.data())[1] = 'i';
  dyn_buf.commit(2);

  EXPECT_EQ(dyn_buf.size(), 2);
  EXPECT_EQ(str, "hi");

  auto data = dyn_buf.data();
  EXPECT_EQ(data.size(), 2);
  const auto* chars = static_cast<const char*>(data.data());
  EXPECT_EQ(chars[0], 'h');
  EXPECT_EQ(chars[1], 'i');
}

// --- buffer_view ---

TEST(BufferTest, buffer_view_default_construction) {
  bnio::async_io::buffer_view view{};

  EXPECT_EQ(view.data, nullptr);
  EXPECT_EQ(view.size, 0);
}

TEST(BufferTest, mutable_buffer_conversion_to_buffer_view) {
  char data[4] = {1, 2, 3, 4};
  bnio::mutable_buffer buf(data, sizeof(data));

  bnio::async_io::buffer_view view = buf.view();

  EXPECT_EQ(view.data, data);
  EXPECT_EQ(view.size, sizeof(data));
}
