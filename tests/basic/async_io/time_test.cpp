#include <bnio/async_io/time.h>
#include <gtest/gtest.h>

#include <chrono>
#include <type_traits>

namespace {

TEST(TimeTest, chrono_aliases) {
  static_assert(
      std::is_same_v<bnio::async_io::steady_clock, std::chrono::steady_clock>);
  static_assert(
      std::is_same_v<bnio::async_io::clock, bnio::async_io::steady_clock>);
  static_assert(
      std::is_same_v<bnio::async_io::system_clock, std::chrono::system_clock>);
  static_assert(std::is_same_v<bnio::async_io::duration,
                               bnio::async_io::clock::duration>);
  static_assert(std::is_same_v<bnio::async_io::time_point,
                               bnio::async_io::clock::time_point>);
  static_assert(std::is_same_v<bnio::async_io::system_time_point,
                               bnio::async_io::system_clock::time_point>);
}

}  // namespace
